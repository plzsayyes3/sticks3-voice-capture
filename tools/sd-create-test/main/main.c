// StickS3 + TF HAT (SKU 9551): ESP-IDF 標準 SDSPI ドライバで
// 「読める / 新規作成できるか」を切り分ける非破壊テスト。
//
// - format_if_mount_failed = false（絶対にフォーマットしない）
// - 生セクタ書き込みはしない
// - 作成するのは /IDFT0926.TST, /IDFF0926.TST, /IDFS0926.TST（各2バイト）と /IDFD0926 ディレクトリのみ

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "stick_s3_board.h"

#define TAG "sdtest"

// HAT 作者の StickS3 サンプルと同じピン
#define PIN_SD_SCK  8
#define PIN_SD_MISO 1
#define PIN_SD_MOSI 0

// Arduino で再現したときと同じ 1MHz から始める
#define SD_FREQ_KHZ 1000

#define MOUNT "/sd"

// M5Unified の M5.begin() は既定で PM1 の BOOST_EN を立てる（setExtOutput(true)）。
// Arduino で読めていた条件に揃えるため、ここでも立てる。
#define M5PM1_REG_PWR_CFG      0x06
#define M5PM1_PWR_CFG_BOOST_EN (1 << 3)

static const char *fr_name(FRESULT r)
{
    static const char *names[] = {
        "FR_OK", "FR_DISK_ERR", "FR_INT_ERR", "FR_NOT_READY", "FR_NO_FILE",
        "FR_NO_PATH", "FR_INVALID_NAME", "FR_DENIED", "FR_EXIST",
        "FR_INVALID_OBJECT", "FR_WRITE_PROTECTED", "FR_INVALID_DRIVE",
        "FR_NOT_ENABLED", "FR_NO_FILESYSTEM", "FR_MKFS_ABORTED", "FR_TIMEOUT",
        "FR_LOCKED", "FR_NOT_ENOUGH_CORE", "FR_TOO_MANY_OPEN_FILES",
        "FR_INVALID_PARAMETER",
    };
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

static void log_card_status(sdmmc_card_t *card, const char *when)
{
    esp_err_t err = sdmmc_get_status(card);  // CMD13
    ESP_LOGI(TAG, "[CMD13 %s] %s", when, esp_err_to_name(err));
}

static void enable_boost(void)
{
    i2c_master_bus_handle_t bus = stick_s3_board_i2c_bus();
    if (!bus) {
        ESP_LOGW(TAG, "I2C bus unavailable; BOOST not touched");
        return;
    }
    i2c_master_dev_handle_t dev;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x6e,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        return;
    }
    uint8_t reg = M5PM1_REG_PWR_CFG, val = 0;
    if (i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 100) == ESP_OK) {
        const uint8_t buf[] = {reg, (uint8_t)(val | M5PM1_PWR_CFG_BOOST_EN)};
        esp_err_t err = i2c_master_transmit(dev, buf, sizeof(buf), 100);
        ESP_LOGI(TAG, "PM1 PWR_CFG 0x%02x -> 0x%02x (%s)", val, buf[1], esp_err_to_name(err));
    }
    i2c_master_bus_rm_device(dev);
}

static void list_root(void)
{
    DIR *d = opendir(MOUNT);
    if (!d) {
        ESP_LOGE(TAG, "opendir failed errno=%d (%s)", errno, strerror(errno));
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        ESP_LOGI(TAG, "  %s %s", e->d_type == DT_DIR ? "[D]" : "   ", e->d_name);
    }
    closedir(d);
}

static void test_read_probe(void)
{
    errno = 0;
    FILE *f = fopen(MOUNT "/PROBE.TXT", "r");
    int e = errno;
    if (!f) {
        ESP_LOGE(TAG, "T1 read PROBE.TXT: fopen NG errno=%d (%s)", e, strerror(e));
        return;
    }
    char buf[16] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    ESP_LOGI(TAG, "T1 read PROBE.TXT: OK %u bytes \"%s\"", (unsigned)n, buf);
}

static void test_append_open_only(void)
{
    // 既存ファイルを追記モードで開いて、何も書かずに閉じる（内容は変わらない）
    errno = 0;
    int fd = open(MOUNT "/PROBE.TXT", O_WRONLY | O_APPEND);
    int e = errno;
    if (fd < 0) {
        ESP_LOGE(TAG, "T2 append-open PROBE.TXT: NG errno=%d (%s)", e, strerror(e));
        return;
    }
    close(fd);
    ESP_LOGI(TAG, "T2 append-open PROBE.TXT: OK (nothing written)");
}

static void test_posix_create(sdmmc_card_t *card)
{
    const char *path = MOUNT "/IDFT0926.TST";
    errno = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int e = errno;
    if (fd < 0) {
        ESP_LOGE(TAG, "T3 POSIX create: open NG errno=%d (%s)", e, strerror(e));
        log_card_status(card, "after T3 fail");
        return;
    }
    ESP_LOGI(TAG, "T3 POSIX create: open OK fd=%d", fd);

    errno = 0;
    ssize_t w = write(fd, "OK", 2);
    e = errno;
    ESP_LOGI(TAG, "T3 write -> %d errno=%d", (int)w, e);
    errno = 0;
    int s = fsync(fd);
    e = errno;
    ESP_LOGI(TAG, "T3 fsync -> %d errno=%d", s, e);
    errno = 0;
    int c = close(fd);
    e = errno;
    ESP_LOGI(TAG, "T3 close -> %d errno=%d", c, e);

    char buf[8] = {0};
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        ESP_LOGI(TAG, "T3 read-back: %u bytes \"%s\" %s", (unsigned)n, buf,
                 strcmp(buf, "OK") == 0 ? "MATCH" : "MISMATCH");
    } else {
        ESP_LOGE(TAG, "T3 read-back fopen NG errno=%d", errno);
    }
}

static void fatfs_create(sdmmc_card_t *card, FIL *fil, const char *tag, const char *name)
{
    // VFS を通さず FatFs を直接呼んで、生の FRESULT を得る
    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    char path[24];
    snprintf(path, sizeof(path), "%u:/%s", pdrv, name);

    ESP_LOGI(TAG, "%s FIL at %p", tag, (void *)fil);
    FRESULT r = f_open(fil, path, FA_WRITE | FA_CREATE_ALWAYS);
    ESP_LOGI(TAG, "%s f_open(%s) -> %d %s", tag, path, r, fr_name(r));
    if (r != FR_OK) {
        log_card_status(card, "after f_open fail");
        return;
    }
    UINT bw = 0;
    r = f_write(fil, "OK", 2, &bw);
    ESP_LOGI(TAG, "%s f_write -> %d %s (bw=%u)", tag, r, fr_name(r), bw);
    r = f_sync(fil);
    ESP_LOGI(TAG, "%s f_sync  -> %d %s", tag, r, fr_name(r));
    r = f_close(fil);
    ESP_LOGI(TAG, "%s f_close -> %d %s", tag, r, fr_name(r));
}

static void test_fatfs_create(sdmmc_card_t *card)
{
    FIL stack_fil;
    fatfs_create(card, &stack_fil, "T4 (stack FIL)", "IDFF0926.TST");

    static FIL static_fil;
    fatfs_create(card, &static_fil, "T4b (static FIL)", "IDFS0926.TST");
}

static void test_mkdir(void)
{
    errno = 0;
    int r = mkdir(MOUNT "/IDFD0926", 0777);
    int e = errno;
    if (r == 0) {
        ESP_LOGI(TAG, "T5 mkdir: OK");
    } else {
        ESP_LOGE(TAG, "T5 mkdir: NG errno=%d (%s)", e, strerror(e));
    }
}

void app_main(void)
{
    // USB シリアルが繋がるまで少し待つ
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "==== SD create test start (SPI %d kHz, CS=none) ====", SD_FREQ_KHZ);

    stick_s3_board_init();
    enable_boost();
    vTaskDelay(pdMS_TO_TICKS(200));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = host.slot;
    dev.gpio_cs = SDSPI_SLOT_NO_CS;

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };

    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &dev, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return;
    }
    sdmmc_card_print_info(stdout, card);

    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &free_b) == ESP_OK) {
        ESP_LOGI(TAG, "FAT total=%llu MB free=%llu MB", total >> 20, free_b >> 20);
    }

    ESP_LOGI(TAG, "root:");
    list_root();

    log_card_status(card, "before tests");
    test_read_probe();
    test_append_open_only();
    test_posix_create(card);
    test_fatfs_create(card);
    test_mkdir();
    log_card_status(card, "after tests");

    ESP_LOGI(TAG, "root after:");
    list_root();

    esp_vfs_fat_sdcard_unmount(MOUNT, card);
    ESP_LOGI(TAG, "==== SD create test done (unmounted) ====");
}
