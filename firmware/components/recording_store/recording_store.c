#include "recording_store.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"

static const char *TAG = "recording_store";

#define STORAGE_LABEL "storage"
#define FLASH_BASE_PATH "/recordings"

/* M5Stack TF HAT (SKU 9551) on the StickS3 HAT header. The HAT ties the
 * card's CS line itself, so the slot runs without a CS GPIO. SPI2 already
 * belongs to the LCD, hence SPI3. The HAT is powered from the 3.3V rail;
 * the PM1 5V boost is not required. */
#define SD_SPI_HOST SPI3_HOST
#define SD_PIN_SCK 8
#define SD_PIN_MISO 1
#define SD_PIN_MOSI 0
#define SD_FREQ_KHZ SDMMC_FREQ_DEFAULT
#define SD_MOUNT_POINT "/sdcard"
#define SD_BASE_PATH SD_MOUNT_POINT "/REC"
#define STORAGE_MAX_FILES 6
#define STORAGE_ALLOCATION_UNIT 4096
#define OGG_MAX_SEGMENTS 255
#define OGG_MAX_PAYLOAD 3072
#define OGG_PACKETS_PER_PAGE 10
#define OGG_SYNC_EVERY_PAGES 5
#define PATH_BUFFER_SIZE 96

typedef struct {
    FILE *file;
    bool active;
    uint32_t sample_rate;
    uint32_t serial;
    uint32_t page_sequence;
    uint64_t granule_pos;
    uint64_t packet_count;
    uint8_t segments[OGG_MAX_SEGMENTS];
    uint8_t payload[OGG_MAX_PAYLOAD];
    size_t segment_count;
    size_t payload_len;
    size_t page_packet_count;
    unsigned pages_since_sync;
    char temp_path[PATH_BUFFER_SIZE];
    char final_path[PATH_BUFFER_SIZE];
} recording_writer_t;

static bool s_mounted;
static bool s_on_sd;
static const char *s_mount_point = FLASH_BASE_PATH;
static const char *s_base_path = FLASH_BASE_PATH;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static sdmmc_card_t *s_sd_card;
static recording_writer_t s_writer;

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *dst, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)(value >> (8 * i));
    }
}

static uint32_t ogg_crc(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : (crc << 1);
        }
    }
    return crc;
}

static esp_err_t sync_file(FILE *file)
{
    if (!file) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fflush(file) != 0) {
        ESP_LOGE(TAG, "fflush failed errno=%d", errno);
        return ESP_FAIL;
    }
    const int fd = fileno(file);
    if (fd < 0 || fsync(fd) != 0) {
        ESP_LOGE(TAG, "fsync failed errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_page(uint8_t header_type,
                            uint64_t granule_pos,
                            const uint8_t *segments,
                            size_t segment_count,
                            const uint8_t *payload,
                            size_t payload_len)
{
    if (!s_writer.file || segment_count > OGG_MAX_SEGMENTS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t header[27 + OGG_MAX_SEGMENTS] = {0};
    memcpy(header, "OggS", 4);
    header[4] = 0;
    header[5] = header_type;
    put_le64(header + 6, granule_pos);
    put_le32(header + 14, s_writer.serial);
    put_le32(header + 18, s_writer.page_sequence++);
    put_le32(header + 22, 0);
    header[26] = (uint8_t)segment_count;
    if (segment_count > 0) {
        memcpy(header + 27, segments, segment_count);
    }

    uint32_t crc = ogg_crc(0, header, 27 + segment_count);
    if (payload_len > 0) {
        crc = ogg_crc(crc, payload, payload_len);
    }
    put_le32(header + 22, crc);

    if (fwrite(header, 1, 27 + segment_count, s_writer.file) != 27 + segment_count) {
        ESP_LOGE(TAG, "write Ogg header failed errno=%d", errno);
        return ESP_FAIL;
    }
    if (payload_len > 0 && fwrite(payload, 1, payload_len, s_writer.file) != payload_len) {
        ESP_LOGE(TAG, "write Ogg payload failed errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_single_packet_page(uint8_t header_type,
                                          uint64_t granule_pos,
                                          const uint8_t *packet,
                                          size_t packet_len)
{
    uint8_t segments[OGG_MAX_SEGMENTS];
    size_t segment_count = 0;
    size_t remaining = packet_len;

    while (remaining >= 255) {
        if (segment_count >= OGG_MAX_SEGMENTS) {
            return ESP_ERR_INVALID_SIZE;
        }
        segments[segment_count++] = 255;
        remaining -= 255;
    }
    if (segment_count >= OGG_MAX_SEGMENTS) {
        return ESP_ERR_INVALID_SIZE;
    }
    segments[segment_count++] = (uint8_t)remaining;

    return write_page(header_type, granule_pos, segments, segment_count, packet, packet_len);
}

static esp_err_t write_opus_headers(uint16_t pre_skip_48k)
{
    uint8_t head[19] = {0};
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = 1;
    put_le16(head + 10, pre_skip_48k);
    put_le32(head + 12, s_writer.sample_rate);
    put_le16(head + 16, 0);
    head[18] = 0;

    esp_err_t err = write_single_packet_page(0x02, 0, head, sizeof(head));
    if (err != ESP_OK) {
        return err;
    }

    static const char vendor[] = "sticks3-voice-capture";
    uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4] = {0};
    memcpy(tags, "OpusTags", 8);
    put_le32(tags + 8, (uint32_t)(sizeof(vendor) - 1));
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    put_le32(tags + 12 + sizeof(vendor) - 1, 0);
    return write_single_packet_page(0, 0, tags, sizeof(tags));
}

static int lacing_count(size_t packet_len)
{
    return (int)(packet_len / 255) + 1;
}

static esp_err_t flush_audio_page(uint8_t header_type)
{
    if (s_writer.segment_count == 0) {
        return ESP_OK;
    }

    esp_err_t err = write_page(header_type,
                               s_writer.granule_pos,
                               s_writer.segments,
                               s_writer.segment_count,
                               s_writer.payload,
                               s_writer.payload_len);
    if (err != ESP_OK) {
        return err;
    }

    s_writer.segment_count = 0;
    s_writer.payload_len = 0;
    s_writer.page_packet_count = 0;
    s_writer.pages_since_sync++;

    if (s_writer.pages_since_sync >= OGG_SYNC_EVERY_PAGES) {
        err = sync_file(s_writer.file);
        if (err != ESP_OK) {
            return err;
        }
        s_writer.pages_since_sync = 0;
    }
    return ESP_OK;
}

static bool storage_partition_is_blank(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        STORAGE_LABEL);
    if (!partition) {
        return false;
    }

    uint8_t buffer[256];
    const size_t inspect = partition->size < 4096 ? partition->size : 4096;
    for (size_t offset = 0; offset < inspect; offset += sizeof(buffer)) {
        size_t chunk = inspect - offset;
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        if (esp_partition_read(partition, offset, buffer, chunk) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < chunk; ++i) {
            if (buffer[i] != 0xff) {
                return false;
            }
        }
    }
    return true;
}

static void log_recoverable_partials(void)
{
    DIR *dir = opendir(s_base_path);
    if (!dir) {
        return;
    }

    unsigned partials = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len >= 5 && strcmp(name + len - 5, ".part") == 0) {
            ESP_LOGW(TAG, "recoverable partial recording: %s/%s", s_base_path, name);
            partials++;
        }
    }
    closedir(dir);

    if (partials > 0) {
        ESP_LOGW(TAG, "%u partial recording(s) retained for inspection", partials);
    }
}

const char *recording_store_base_path(void)
{
    return s_base_path;
}

bool recording_store_on_sd(void)
{
    return s_on_sd;
}

esp_err_t recording_store_get_usage(uint64_t *used_bytes, uint64_t *capacity_bytes)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0;
    uint64_t free_space = 0;
    esp_err_t err = esp_vfs_fat_info(s_mount_point, &total, &free_space);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_info failed: %s", esp_err_to_name(err));
        return err;
    }

    if (capacity_bytes) {
        *capacity_bytes = total;
    }
    if (used_bytes) {
        *used_bytes = total > free_space ? total - free_space : 0;
    }
    return ESP_OK;
}

static esp_err_t mount_sd(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_config, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SD_SPI_HOST;
    slot_config.gpio_cs = SDSPI_SLOT_NO_CS;

    /* Never format the card: it may hold the user's own files, and a
     * missing/unreadable card simply falls back to internal flash. */
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = STORAGE_MAX_FILES,
        .allocation_unit_size = 0,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(err));
        spi_bus_free(SD_SPI_HOST);
        s_sd_card = NULL;
        return err;
    }

    if (mkdir(SD_BASE_PATH, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "create %s failed errno=%d", SD_BASE_PATH, errno);
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_sd_card);
        spi_bus_free(SD_SPI_HOST);
        s_sd_card = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SD card %s, %" PRIu64 " MB",
             s_sd_card->cid.name,
             ((uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size) >> 20);
    return ESP_OK;
}

static esp_err_t mount_flash(bool allow_format)
{
    const bool blank_partition = storage_partition_is_blank();
    esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = allow_format && blank_partition,
        .max_files = STORAGE_MAX_FILES,
        .allocation_unit_size = STORAGE_ALLOCATION_UNIT,
    };

    if (blank_partition) {
        ESP_LOGW(TAG, "blank recording partition detected; first mount may format FAT");
    }

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        FLASH_BASE_PATH, STORAGE_LABEL, &config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "mount storage failed without destructive retry: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t recording_store_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    if (mount_sd() == ESP_OK) {
        s_on_sd = true;
        s_mount_point = SD_MOUNT_POINT;
        s_base_path = SD_BASE_PATH;
    } else {
        esp_err_t err = mount_flash(true);
        if (err != ESP_OK) {
            return err;
        }
        s_on_sd = false;
        s_mount_point = FLASH_BASE_PATH;
        s_base_path = FLASH_BASE_PATH;
    }

    s_mounted = true;
    log_recoverable_partials();
    ESP_LOGI(TAG, "recording storage mounted at %s (%s)",
             s_base_path, s_on_sd ? "SD card" : "internal flash");
    return ESP_OK;
}

const char *recording_store_open_flash_backlog(void)
{
    /* Only meaningful while recordings go to the SD card: internal flash may
     * still hold recordings made before the card was fitted. Never formats;
     * a blank partition simply has nothing to sync. */
    if (!s_mounted || !s_on_sd || s_wl_handle != WL_INVALID_HANDLE) {
        return NULL;
    }
    if (storage_partition_is_blank()) {
        return NULL;
    }
    if (mount_flash(false) != ESP_OK) {
        s_wl_handle = WL_INVALID_HANDLE;
        return NULL;
    }
    ESP_LOGI(TAG, "internal flash backlog mounted at %s", FLASH_BASE_PATH);
    return FLASH_BASE_PATH;
}

void recording_store_close_flash_backlog(void)
{
    if (!s_on_sd || s_wl_handle == WL_INVALID_HANDLE) {
        return;
    }
    esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(FLASH_BASE_PATH, s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unmount flash backlog failed: %s", esp_err_to_name(err));
    }
    s_wl_handle = WL_INVALID_HANDLE;
}

esp_err_t recording_store_begin(uint32_t session_id,
                                uint32_t input_sample_rate,
                                uint16_t pre_skip_48k)
{
    if (!s_mounted || s_writer.active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (input_sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_writer, 0, sizeof(s_writer));
    s_writer.sample_rate = input_sample_rate;
    s_writer.serial = esp_random();
    s_writer.granule_pos = pre_skip_48k;

    bool found_name = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t suffix = esp_random();
        int temp_len = snprintf(s_writer.temp_path, sizeof(s_writer.temp_path),
                                "%s/%08" PRIu32 "-%08" PRIx32 ".part",
                                s_base_path, session_id, suffix);
        int final_len = snprintf(s_writer.final_path, sizeof(s_writer.final_path),
                                 "%s/%08" PRIu32 "-%08" PRIx32 ".ogg",
                                 s_base_path, session_id, suffix);
        if (temp_len <= 0 || final_len <= 0 ||
            temp_len >= (int)sizeof(s_writer.temp_path) ||
            final_len >= (int)sizeof(s_writer.final_path)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (access(s_writer.temp_path, F_OK) != 0 &&
            access(s_writer.final_path, F_OK) != 0) {
            found_name = true;
            break;
        }
    }
    if (!found_name) {
        return ESP_ERR_INVALID_STATE;
    }

    s_writer.file = fopen(s_writer.temp_path, "wb");
    if (!s_writer.file) {
        ESP_LOGE(TAG, "create %s failed errno=%d", s_writer.temp_path, errno);
        return ESP_FAIL;
    }
    s_writer.active = true;

    esp_err_t err = write_opus_headers(pre_skip_48k);
    if (err == ESP_OK) {
        err = sync_file(s_writer.file);
    }
    if (err != ESP_OK) {
        recording_store_abort(false);
        return err;
    }

    ESP_LOGI(TAG, "recording opened: %s", s_writer.temp_path);
    return ESP_OK;
}

esp_err_t recording_store_write_opus(const uint8_t *packet,
                                     size_t packet_len,
                                     uint32_t frame_samples)
{
    if (!s_writer.active || !s_writer.file || !packet || packet_len == 0 || frame_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int needed_segments = lacing_count(packet_len);
    if (needed_segments <= 0 || needed_segments > OGG_MAX_SEGMENTS || packet_len > OGG_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_writer.page_packet_count >= OGG_PACKETS_PER_PAGE ||
        s_writer.segment_count + (size_t)needed_segments > OGG_MAX_SEGMENTS ||
        s_writer.payload_len + packet_len > OGG_MAX_PAYLOAD) {
        esp_err_t err = flush_audio_page(0);
        if (err != ESP_OK) {
            return err;
        }
    }

    size_t remaining = packet_len;
    while (remaining >= 255) {
        s_writer.segments[s_writer.segment_count++] = 255;
        remaining -= 255;
    }
    s_writer.segments[s_writer.segment_count++] = (uint8_t)remaining;

    memcpy(s_writer.payload + s_writer.payload_len, packet, packet_len);
    s_writer.payload_len += packet_len;
    s_writer.page_packet_count++;
    s_writer.packet_count++;
    s_writer.granule_pos += ((uint64_t)frame_samples * 48000ULL) / s_writer.sample_rate;
    return ESP_OK;
}

esp_err_t recording_store_finish(long *out_size)
{
    if (!s_writer.active || !s_writer.file) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_writer.packet_count == 0) {
        ESP_LOGW(TAG, "discarding empty recording %s", s_writer.temp_path);
        recording_store_abort(false);
        if (out_size) {
            *out_size = 0;
        }
        return ESP_OK;
    }

    esp_err_t err = flush_audio_page(0x04);
    if (err == ESP_OK) {
        err = sync_file(s_writer.file);
    }

    long size = 0;
    if (err == ESP_OK) {
        if (fseek(s_writer.file, 0, SEEK_END) != 0) {
            err = ESP_FAIL;
        } else {
            size = ftell(s_writer.file);
            if (size <= 0) {
                err = ESP_FAIL;
            }
        }
    }

    FILE *file = s_writer.file;
    s_writer.file = NULL;
    if (file && fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        if (access(s_writer.final_path, F_OK) == 0) {
            ESP_LOGE(TAG, "refusing to overwrite %s", s_writer.final_path);
            err = ESP_ERR_INVALID_STATE;
        } else if (rename(s_writer.temp_path, s_writer.final_path) != 0) {
            ESP_LOGE(TAG, "rename %s -> %s failed errno=%d",
                     s_writer.temp_path, s_writer.final_path, errno);
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "recording finalized: %s (%ld bytes)", s_writer.final_path, size);
        if (out_size) {
            *out_size = size;
        }
    } else {
        ESP_LOGE(TAG, "recording finalize failed; partial retained at %s", s_writer.temp_path);
    }

    s_writer.active = false;
    return err;
}

void recording_store_abort(bool keep_partial)
{
    if (!s_writer.active) {
        return;
    }

    if (s_writer.file) {
        (void)sync_file(s_writer.file);
        fclose(s_writer.file);
        s_writer.file = NULL;
    }

    if (!keep_partial && s_writer.temp_path[0]) {
        (void)remove(s_writer.temp_path);
    } else if (keep_partial && s_writer.temp_path[0]) {
        ESP_LOGW(TAG, "partial recording retained: %s", s_writer.temp_path);
    }
    s_writer.active = false;
}

bool recording_store_active(void)
{
    return s_writer.active;
}

const char *recording_store_current_path(void)
{
    return s_writer.active ? s_writer.temp_path : "";
}
