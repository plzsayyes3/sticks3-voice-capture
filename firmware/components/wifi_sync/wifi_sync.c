#include "wifi_sync.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "recording_store.h"
#include "secrets.h"

static const char *TAG = "wifi_sync";

#define WIFI_CONNECT_TIMEOUT_MS 8000
#define UPLOAD_CHUNK_SIZE 4096
#define SENT_SUFFIX ".sent"
#define PATH_BUFFER_SIZE 320
/* Recording filenames are "<8-hex-id>-<8-hex-suffix>.ogg" (~22 bytes) or
 * that plus SENT_SUFFIX; 40 leaves comfortable margin without the bloat of
 * reusing PATH_BUFFER_SIZE (sized for a full URL) per entry. */
#define SYNC_FILENAME_BUFFER_SIZE 40
/* Bounds the scratch array so the directory listing can be collected in one
 * pass (see collect_sync_candidates) instead of mutating entries while
 * readdir() is still iterating the directory, which is unsafe on FATFS
 * (entries can be skipped or re-visited). 48 is far above what the
 * ~3.94MB storage partition holds at typical recording sizes. */
#define MAX_SYNC_CANDIDATES 48

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static bool s_initialized;
static atomic_bool s_sync_running;
static EventGroupHandle_t s_wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sync_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL),
        TAG, "register wifi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL),
        TAG, "register ip event handler");

    /* Deliberately not calling esp_wifi_init()/start() here: even without
     * starting the radio, esp_wifi_init() permanently reserves tens of KB
     * of internal RAM for its RX/TX buffers, which starved the audio
     * pipeline's 32KB task-stack allocation (ESP_ERR_NO_MEM on recording
     * start). The whole Wi-Fi driver lifecycle — init, start, stop, deinit
     * — now lives inside wifi_sync_task, so that memory only exists for
     * the duration of an actual Sync. */

    s_initialized = true;
    return ESP_OK;
}

static bool connect_to_known_network(void)
{
    for (size_t i = 0; i < STICKS3_WIFI_NETWORK_COUNT; ++i) {
        const sticks3_wifi_network_t *net = &STICKS3_WIFI_NETWORKS[i];

        wifi_config_t wifi_config = {0};
        strlcpy((char *)wifi_config.sta.ssid, net->ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, net->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = strlen(net->password) == 0
            ? WIFI_AUTH_OPEN
            : WIFI_AUTH_WPA2_PSK;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
            continue;
        }
        ESP_LOGI(TAG, "attempting Wi-Fi network %u/%u: %s",
                 (unsigned)(i + 1), (unsigned)STICKS3_WIFI_NETWORK_COUNT, net->ssid);
        if (esp_wifi_connect() != ESP_OK) {
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "connected to %s", net->ssid);
            return true;
        }

        ESP_LOGW(TAG, "network %s not reachable, trying next", net->ssid);
        esp_wifi_disconnect();
    }
    return false;
}

static bool has_suffix(const char *name, const char *suffix)
{
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (name_len < suffix_len) {
        return false;
    }
    return strcmp(name + name_len - suffix_len, suffix) == 0;
}

static bool is_pending_recording(const char *name)
{
    return has_suffix(name, ".ogg") && !has_suffix(name, SENT_SUFFIX);
}

/* Recordings synced by firmware before this change were left behind as
 * "<id>.ogg.sent" (renamed, never deleted) and would otherwise sit on the
 * ~3.94MB storage partition forever. These are already confirmed uploaded
 * by definition of having been renamed, so sync can just clear them out. */
static bool is_stale_sent_file(const char *name)
{
    return has_suffix(name, SENT_SUFFIX);
}

/* "00000002-8dbbe763.ogg" -> "00000002-8dbbe763" */
static void recording_id_from_filename(const char *name, char *out, size_t out_size)
{
    size_t len = strlen(name);
    size_t ogg_len = strlen(".ogg");
    size_t id_len = (len > ogg_len) ? len - ogg_len : 0;
    if (id_len >= out_size) {
        id_len = out_size - 1;
    }
    memcpy(out, name, id_len);
    out[id_len] = '\0';
}

static esp_err_t upload_file(const char *path, const char *recording_id)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "open %s for upload failed", path);
        return ESP_FAIL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        return ESP_FAIL;
    }

    char url[PATH_BUFFER_SIZE];
    snprintf(url, sizeof(url), "%s/v1/recordings", STICKS3_RECEIVER_URL);
    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", STICKS3_DEVICE_TOKEN);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "X-Recording-ID", recording_id);
    esp_http_client_set_header(client, "Content-Type", "audio/ogg");

    esp_err_t err = esp_http_client_open(client, (int)size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed for %s: %s", recording_id, esp_err_to_name(err));
        fclose(file);
        esp_http_client_cleanup(client);
        return err;
    }

    uint8_t buffer[UPLOAD_CHUNK_SIZE];
    size_t remaining = (size_t)size;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t read = fread(buffer, 1, chunk, file);
        if (read == 0) {
            err = ESP_FAIL;
            break;
        }
        int written = esp_http_client_write(client, (const char *)buffer, (int)read);
        if (written < 0 || (size_t)written != read) {
            err = ESP_FAIL;
            break;
        }
        remaining -= read;
    }
    fclose(file);

    int status_code = -1;
    if (err == ESP_OK) {
        int content_length = esp_http_client_fetch_headers(client);
        (void)content_length;
        status_code = esp_http_client_get_status_code(client);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status_code != 200 && status_code != 201) {
        ESP_LOGE(TAG, "upload of %s rejected: HTTP %d", recording_id, status_code);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "uploaded %s (HTTP %d)", recording_id, status_code);
    return ESP_OK;
}

/* Two-pass sync: collect filenames first (readdir only), then act on them
 * (upload/delete) after closing the directory. Renaming or deleting entries
 * while readdir() is still walking the same directory is unsafe on FATFS —
 * the traversal can skip or re-visit entries depending on how the directory
 * cluster chain shifts underneath it. */
typedef struct {
    char pending[MAX_SYNC_CANDIDATES][SYNC_FILENAME_BUFFER_SIZE];
    size_t pending_count;
    char stale_sent[MAX_SYNC_CANDIDATES][SYNC_FILENAME_BUFFER_SIZE];
    size_t stale_sent_count;
} sync_candidates_t;

static void collect_sync_candidates(const char *base_path, sync_candidates_t *out)
{
    memset(out, 0, sizeof(*out));

    DIR *dir = opendir(base_path);
    if (!dir) {
        ESP_LOGW(TAG, "cannot open %s for sync", base_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (is_pending_recording(entry->d_name)) {
            if (out->pending_count < MAX_SYNC_CANDIDATES) {
                strlcpy(out->pending[out->pending_count], entry->d_name, SYNC_FILENAME_BUFFER_SIZE);
                out->pending_count++;
            } else {
                ESP_LOGW(TAG, "sync: more than %u pending recordings; %s will sync next time",
                         (unsigned)MAX_SYNC_CANDIDATES, entry->d_name);
            }
        } else if (is_stale_sent_file(entry->d_name)) {
            if (out->stale_sent_count < MAX_SYNC_CANDIDATES) {
                strlcpy(out->stale_sent[out->stale_sent_count], entry->d_name, SYNC_FILENAME_BUFFER_SIZE);
                out->stale_sent_count++;
            }
        }
    }
    closedir(dir);
}

static void sync_pending_recordings(void)
{
    const char *base_path = recording_store_base_path();

    /* Heap-allocated and freed within this call rather than static/global:
     * a static sync_candidates_t here would permanently reserve ~3.8KB of
     * internal RAM for the firmware's whole lifetime just for the rare
     * moments a sync runs — the same class of internal-RAM pressure that
     * caused the ESP_ERR_NO_MEM regression this project already hit once
     * (see canonical doc). Sync only ever runs outside of active recording,
     * so a transient allocation here is safe. */
    sync_candidates_t *candidates = malloc(sizeof(sync_candidates_t));
    if (!candidates) {
        ESP_LOGE(TAG, "sync: out of memory collecting candidates");
        return;
    }
    collect_sync_candidates(base_path, candidates);

    for (size_t i = 0; i < candidates->stale_sent_count; ++i) {
        char full_path[PATH_BUFFER_SIZE];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, candidates->stale_sent[i]);
        if (remove(full_path) != 0) {
            ESP_LOGW(TAG, "failed to clear stale synced file %s", candidates->stale_sent[i]);
        } else {
            ESP_LOGI(TAG, "cleared stale synced file %s", candidates->stale_sent[i]);
        }
    }

    unsigned uploaded = 0;
    unsigned failed = 0;
    for (size_t i = 0; i < candidates->pending_count; ++i) {
        char full_path[PATH_BUFFER_SIZE];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, candidates->pending[i]);

        char recording_id[PATH_BUFFER_SIZE];
        recording_id_from_filename(candidates->pending[i], recording_id, sizeof(recording_id));

        if (upload_file(full_path, recording_id) == ESP_OK) {
            /* The receiver fsyncs the recording durably and dedupes by
             * SHA-256 before replying 200/201, so a success response means
             * the recording is safe on the Mac. Delete rather than rename
             * to .sent: the storage partition is only ~3.94MB, and files
             * that are merely renamed never free that space. */
            if (remove(full_path) != 0) {
                ESP_LOGW(TAG, "uploaded %s but delete failed; will retry next sync",
                         candidates->pending[i]);
            }
            uploaded++;
        } else {
            ESP_LOGW(TAG, "upload failed for %s; left on device for retry", candidates->pending[i]);
            failed++;
        }
    }

    ESP_LOGI(TAG, "sync complete: %u uploaded, %u failed, %u stale .sent cleared",
             uploaded, failed, (unsigned)candidates->stale_sent_count);
    free(candidates);
}

static void wifi_sync_task(void *arg)
{
    (void)arg;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        atomic_store(&s_sync_running, false);
        vTaskDelete(NULL);
        return;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        atomic_store(&s_sync_running, false);
        vTaskDelete(NULL);
        return;
    }

    if (connect_to_known_network()) {
        sync_pending_recordings();
        esp_wifi_disconnect();
    } else {
        ESP_LOGW(TAG, "no known Wi-Fi network in range; recordings stay on device");
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    atomic_store(&s_sync_running, false);
    vTaskDelete(NULL);
}

esp_err_t wifi_sync_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_sync_running, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(wifi_sync_task, "wifi_sync", 8192, NULL, 4, NULL, 0);
    if (ok != pdPASS) {
        atomic_store(&s_sync_running, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool wifi_sync_is_running(void)
{
    return atomic_load(&s_sync_running);
}
