#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button_gpio.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"
#include "hal/gpio_types.h"
#include "iot_button.h"

#include "audio_pipeline.h"
#include "recording_store.h"
#include "stick_s3_board.h"
#include "ui_status.h"
#include "wifi_sync.h"
#include "voice_ble.h"

static const char *TAG = "voice_stick";

#define BATTERY_REFRESH_FALLBACK_MS (10 * 1000)
#define DISPLAY_DIM_TIMEOUT_MS (30 * 1000)
#define DISPLAY_ACTIVE_BRIGHTNESS 128
#define DISPLAY_DIM_BRIGHTNESS 32
#define DISPLAY_DIM_TIMEOUT_US (DISPLAY_DIM_TIMEOUT_MS * 1000ULL)
#define BATTERY_REFRESH_FALLBACK_US (BATTERY_REFRESH_FALLBACK_MS * 1000ULL)
#define DEEP_SLEEP_TIMEOUT_MS (5 * 60 * 1000)
#define DEEP_SLEEP_TIMEOUT_US (DEEP_SLEEP_TIMEOUT_MS * 1000ULL)
#define SIDE_LONG_PRESS_MS 700
#define CAPACITY_DISPLAY_TIMEOUT_MS (4 * 1000)
#define CAPACITY_DISPLAY_TIMEOUT_US (CAPACITY_DISPLAY_TIMEOUT_MS * 1000ULL)
#define SYNC_RESULT_DISPLAY_TIMEOUT_MS (2 * 1000)
#define SYNC_RESULT_DISPLAY_TIMEOUT_US (SYNC_RESULT_DISPLAY_TIMEOUT_MS * 1000ULL)

static bool s_recording;
static bool s_ota_updating;
static bool s_display_dimmed;
static bool s_recording_pm_locked;
static bool s_ota_pm_locked;
static bool s_battery_charging;
static bool s_usb_powered;
static esp_pm_lock_handle_t s_cpu_freq_lock;
static esp_timer_handle_t s_display_dim_timer;
static esp_timer_handle_t s_deep_sleep_timer;
static esp_timer_handle_t s_battery_refresh_timer;
static esp_timer_handle_t s_host_response_timer;
static esp_timer_handle_t s_capacity_display_timer;
static esp_timer_handle_t s_sync_result_display_timer;
static uint32_t s_session_id = 1;
static QueueHandle_t s_app_event_queue;
static button_handle_t s_front_button;
static button_handle_t s_side_button;
static int64_t s_primary_down_us;
static int64_t s_secondary_down_us;
static uint32_t s_primary_session_id;

typedef enum {
    APP_UI_STATE_READY,
    APP_UI_STATE_RECORDING,
    APP_UI_STATE_THINKING,
    APP_UI_STATE_PENDING_CONFIRMATION,
    APP_UI_STATE_ERROR,
} app_ui_state_t;

static app_ui_state_t s_app_ui_state = APP_UI_STATE_READY;

typedef enum {
    INTERACTION_MODE_HOLD_TO_TALK,
    INTERACTION_MODE_CLICK_TO_TALK,
} interaction_mode_t;

static interaction_mode_t s_interaction_mode = INTERACTION_MODE_CLICK_TO_TALK;

static const char *app_ui_state_name(app_ui_state_t state)
{
    switch (state) {
    case APP_UI_STATE_READY:
        return "ready";
    case APP_UI_STATE_RECORDING:
        return "recording";
    case APP_UI_STATE_THINKING:
        return "thinking";
    case APP_UI_STATE_PENDING_CONFIRMATION:
        return "pending_confirmation";
    case APP_UI_STATE_ERROR:
        return "error";
    }
    return "unknown";
}

typedef enum {
    APP_EVENT_FRONT_DOWN,
    APP_EVENT_FRONT_UP,
    APP_EVENT_SIDE_DOWN,
    APP_EVENT_SIDE_UP,
    APP_EVENT_UI_STATE,
    APP_EVENT_BLE_CONNECTED,
    APP_EVENT_BLE_DISCONNECTED,
    APP_EVENT_AUDIO_ERROR,
    APP_EVENT_POWER_IRQ,
    APP_EVENT_BATTERY_REFRESH,
    APP_EVENT_ENTER_DEEP_SLEEP,
    APP_EVENT_OTA_BEGIN,
    APP_EVENT_OTA_PROGRESS,
    APP_EVENT_OTA_DONE,
    APP_EVENT_OTA_END,
    APP_EVENT_HOST_RESPONSE_TIMEOUT,
    APP_EVENT_WIFI_SYNC_DONE,
    APP_EVENT_WIFI_CONNECTED,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    uint32_t written;
    uint32_t size;
    esp_err_t error;
    char state[32];
    char text[96];
} app_event_t;

static void update_battery_status(void);
static void queue_app_event(app_event_type_t type);
static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size);
static void queue_ui_state_event(const char *state, const char *text);
static void apply_interaction_mode(interaction_mode_t mode);
static void show_storage_capacity(void);
static void refresh_storage_label(void);
static void show_sync_result(wifi_sync_result_t result, unsigned uploaded, unsigned failed);

static bool is_external_powered(void)
{
    return s_battery_charging || s_usb_powered;
}

static esp_err_t init_power_management(void)
{
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "recording_cpu", &s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t acquire_recording_pm_locks(void)
{
    if (s_recording_pm_locked) {
        return ESP_OK;
    }

    esp_err_t err = esp_pm_lock_acquire(s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }

    s_recording_pm_locked = true;
    return ESP_OK;
}

static void release_recording_pm_locks(void)
{
    if (!s_recording_pm_locked) {
        return;
    }

    (void)esp_pm_lock_release(s_cpu_freq_lock);
    s_recording_pm_locked = false;
}

static esp_err_t acquire_ota_pm_locks(void)
{
    if (s_ota_pm_locked) {
        return ESP_OK;
    }

    esp_err_t err = esp_pm_lock_acquire(s_cpu_freq_lock);
    if (err != ESP_OK) {
        return err;
    }

    s_ota_pm_locked = true;
    return ESP_OK;
}

static void release_ota_pm_locks(void)
{
    if (!s_ota_pm_locked) {
        return;
    }

    (void)esp_pm_lock_release(s_cpu_freq_lock);
    s_ota_pm_locked = false;
}

static void restart_display_dim_timer(void)
{
    if (!s_display_dim_timer) {
        return;
    }

    (void)esp_timer_stop(s_display_dim_timer);
    if (!s_recording && !s_ota_updating) {
        esp_err_t err = esp_timer_start_once(s_display_dim_timer, DISPLAY_DIM_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start dim timer failed: %s", esp_err_to_name(err));
        }
    }
}

static void restart_deep_sleep_timer(void)
{
    if (!s_deep_sleep_timer) {
        return;
    }

    (void)esp_timer_stop(s_deep_sleep_timer);
    if (!s_recording && !s_ota_updating && !is_external_powered()) {
        esp_err_t err = esp_timer_start_once(s_deep_sleep_timer, DEEP_SLEEP_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start deep sleep timer failed: %s", esp_err_to_name(err));
        }
    } else if (is_external_powered()) {
        ESP_LOGD(TAG, "deep sleep timer paused while external power is present");
    }
}

static void note_activity(void)
{
    if (s_display_dimmed) {
        esp_err_t err = ui_status_set_brightness(DISPLAY_ACTIVE_BRIGHTNESS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore brightness failed: %s", esp_err_to_name(err));
        } else {
            s_display_dimmed = false;
            ui_status_set_idle_dimmed(false);
        }
    }
    restart_display_dim_timer();
    restart_deep_sleep_timer();
}

static void stop_host_response_timer(void)
{
    if (s_host_response_timer) {
        (void)esp_timer_stop(s_host_response_timer);
    }
}

static void enter_deep_sleep(void)
{
    if (s_recording || s_ota_updating || voice_ble_ota_is_active() || wifi_sync_is_running()) {
        restart_deep_sleep_timer();
        return;
    }

    if (is_external_powered()) {
        ESP_LOGI(TAG, "skip deep sleep while charging or USB powered");
        restart_deep_sleep_timer();
        return;
    }

    bool charging = false;
    bool usb_powered = false;
    esp_err_t power_err = stick_s3_board_battery_charging(&charging);
    if (power_err == ESP_OK) {
        power_err = stick_s3_board_usb_powered(&usb_powered);
    }
    if (power_err == ESP_OK && (charging || usb_powered)) {
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ESP_LOGI(TAG, "skip deep sleep after fresh power check charging=%d usb=%d",
                 charging, usb_powered);
        restart_deep_sleep_timer();
        return;
    }

    const gpio_num_t wake_gpio = STICK_S3_PIN_BUTTON_FRONT;
    if (!esp_sleep_is_valid_wakeup_gpio(wake_gpio)) {
        ESP_LOGE(TAG, "GPIO%d cannot wake from deep sleep", wake_gpio);
        restart_deep_sleep_timer();
        return;
    }

    if (gpio_get_level(wake_gpio) == 0) {
        ESP_LOGI(TAG, "skip deep sleep: front button is pressed");
        restart_deep_sleep_timer();
        return;
    }

    ESP_LOGI(TAG, "entering deep sleep, wake on front button GPIO%d low (level=%d)",
             wake_gpio, gpio_get_level(wake_gpio));
    release_recording_pm_locks();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_status_set_brightness(0));
    ui_status_prepare_deep_sleep();
    stick_s3_board_prepare_deep_sleep();

    /* Clear any stale wakeup source bits left over from light sleep / esp_pm
       configuration (e.g. gpio_wakeup_enable on the PMIC IRQ line). Without
       this the chip can wake immediately from an unrelated trigger. */
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    /* Keep RTC peripherals powered so the internal pull-up on the wake pin
       remains effective in deep sleep; combined with the explicit RTC pull-up
       below this prevents GPIO%d from floating low and self-waking. */
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    (void)rtc_gpio_pulldown_dis(wake_gpio);
    (void)rtc_gpio_pullup_en(wake_gpio);

    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << wake_gpio,
                                                    ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable deep sleep wake failed: %s", esp_err_to_name(err));
        restart_deep_sleep_timer();
        return;
    }

    /* Wait for the wake pin to settle high (button release bounce, parasitic
       capacitance, etc.). If it stays low we would just wake up immediately
       after esp_deep_sleep_start(), so abort and retry later. */
    int wait_ms = 0;
    while (gpio_get_level(wake_gpio) == 0 && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (gpio_get_level(wake_gpio) == 0) {
        ESP_LOGW(TAG, "front button still low after %d ms, abort deep sleep", wait_ms);
        restart_deep_sleep_timer();
        return;
    }

    ESP_LOGI(TAG, "deep sleep go (wait_ms=%d level=%d)", wait_ms,
             gpio_get_level(wake_gpio));
    esp_deep_sleep_start();
}

static bool app_ui_allows_recording_start(void)
{
    return s_app_ui_state != APP_UI_STATE_PENDING_CONFIRMATION;
}

static uint32_t start_recording(void)
{
    ESP_LOGW(TAG, "HEAP diag: free=%u largest_free_block=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    const bool ble_ready = voice_ble_is_ready();
    const bool ota_active = voice_ble_ota_is_active();
    const bool ui_allows_start = app_ui_allows_recording_start();
    const bool sync_active = wifi_sync_is_running();
    if (s_recording || s_ota_updating || ota_active || !ui_allows_start || sync_active) {
        ESP_LOGW(TAG,
                 "start recording denied: recording=%d ota=%d ble_ota=%d ble_ready=%d ui_state=%d sync=%d",
                 s_recording, s_ota_updating, ota_active, ble_ready, s_app_ui_state, sync_active);
        return 0;
    }

    const uint32_t session_id = s_session_id++;
    esp_err_t err = acquire_recording_pm_locks();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "acquire recording pm locks failed: %s", esp_err_to_name(err));
        s_app_ui_state = APP_UI_STATE_ERROR;
        ui_status_set_error("でんきのじゅんびができないよ");
        return 0;
    }

    err = audio_pipeline_start(session_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(err));
        release_recording_pm_locks();
        s_app_ui_state = APP_UI_STATE_ERROR;
        ui_status_set_error("マイクがうごかないよ");
        return 0;
    }

    s_recording = true;
    s_app_ui_state = APP_UI_STATE_RECORDING;
    restart_display_dim_timer();
    restart_deep_sleep_timer();
    ui_status_set_recording(session_id);
    return session_id;
}

static uint32_t stop_recording(void)
{
    if (!s_recording) {
        return 0;
    }

    const uint32_t session_id = audio_pipeline_session_id();
    s_recording = false;
    esp_err_t err = audio_pipeline_stop();
    release_recording_pm_locks();
    restart_display_dim_timer();
    restart_deep_sleep_timer();

    refresh_storage_label();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio stop failed: %s", esp_err_to_name(err));
        s_app_ui_state = APP_UI_STATE_ERROR;
        ui_status_set_error("ろくおんできなかったよ");
    } else if (!voice_ble_is_ready()) {
        s_app_ui_state = APP_UI_STATE_READY;
        ui_status_set_idle();
        note_activity();
    }
    return session_id;
}

static void queue_app_event(app_event_type_t type)
{
    queue_app_event_with_ota(type, 0, 0);
}

static void queue_app_event_with_ota(app_event_type_t type, uint32_t written, uint32_t size)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .written = written,
            .size = size,
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

static void queue_wifi_sync_done_event(wifi_sync_result_t result, unsigned uploaded, unsigned failed)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = APP_EVENT_WIFI_SYNC_DONE,
            .written = uploaded,
            .size = failed,
            /* Reusing .error (esp_err_t) to carry wifi_sync_result_t here,
             * not an actual esp_err_t — APP_EVENT_WIFI_SYNC_DONE is the
             * only handler that reads it, and it casts back. */
            .error = (esp_err_t)result,
        };
        (void)xQueueSend(s_app_event_queue, &event, 0);
    }
}

/* Runs on the wifi_sync task, not the main event-loop task — must only
 * queue an event, never touch UI or app state directly. */
static void wifi_sync_done_cb(wifi_sync_result_t result, unsigned uploaded, unsigned failed)
{
    queue_wifi_sync_done_event(result, uploaded, failed);
}

/* Same threading rule as wifi_sync_done_cb. */
static void wifi_sync_connected_cb(void)
{
    queue_app_event(APP_EVENT_WIFI_CONNECTED);
}

static void queue_app_event_from_isr(app_event_type_t type, BaseType_t *high_task_woken)
{
    if (s_app_event_queue) {
        app_event_t event = {
            .type = type,
            .written = 0,
            .size = 0,
        };
        (void)xQueueSendFromISR(s_app_event_queue, &event, high_task_woken);
    }
}

static void queue_ui_state_event(const char *state, const char *text)
{
    if (!s_app_event_queue) {
        ESP_LOGW(TAG, "drop ui_state state=%s text_len=%u: app queue unavailable",
                 state ? state : "nil",
                 (unsigned)(text ? strlen(text) : 0));
        return;
    }

    app_event_t event = {
        .type = APP_EVENT_UI_STATE,
    };
    if (state) {
        strlcpy(event.state, state, sizeof(event.state));
    }
    if (text) {
        strlcpy(event.text, text, sizeof(event.text));
    }
    ESP_LOGI(TAG, "queue ui_state state=%s text_len=%u current=%s recording=%d",
             event.state[0] ? event.state : "nil",
             (unsigned)strlen(event.text),
             app_ui_state_name(s_app_ui_state),
             s_recording);
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop ui_state state=%s: app queue full",
                 event.state[0] ? event.state : "nil");
    }
}

static void audio_pipeline_error_cb(esp_err_t error)
{
    if (!s_app_event_queue) {
        ESP_LOGE(TAG, "recording failed before app queue ready: %s", esp_err_to_name(error));
        return;
    }

    app_event_t event = {
        .type = APP_EVENT_AUDIO_ERROR,
        .error = error,
    };
    if (xQueueSend(s_app_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "drop recording error event: %s", esp_err_to_name(error));
    }
}

static void front_button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_FRONT_DOWN);
}

static void front_button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_FRONT_UP);
}

static void side_button_down_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_SIDE_DOWN);
}

static void side_button_up_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    queue_app_event(APP_EVENT_SIDE_UP);
}

static void ble_connection_cb(bool connected)
{
    queue_app_event(connected ? APP_EVENT_BLE_CONNECTED : APP_EVENT_BLE_DISCONNECTED);
}

static void ble_control_cb(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "ignore invalid control json");
        return;
    }

    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (cJSON_IsString(event) && strcmp(event->valuestring, "ui_state") == 0 &&
        cJSON_IsString(state)) {
        queue_ui_state_event(state->valuestring, cJSON_IsString(text) ? text->valuestring : "");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "interaction_mode") == 0 &&
               cJSON_IsString(mode)) {
        if (strcmp(mode->valuestring, "click_to_talk") == 0) {
            apply_interaction_mode(INTERACTION_MODE_CLICK_TO_TALK);
        } else if (strcmp(mode->valuestring, "hold_to_talk") == 0) {
            apply_interaction_mode(INTERACTION_MODE_HOLD_TO_TALK);
        } else {
            ESP_LOGW(TAG, "unknown interaction_mode %s", mode->valuestring);
        }
    }
    cJSON_Delete(root);
}

static uint32_t elapsed_button_ms(int64_t down_us)
{
    if (down_us <= 0) {
        return 0;
    }
    int64_t elapsed_us = esp_timer_get_time() - down_us;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }
    return (uint32_t)(elapsed_us / 1000);
}

static void apply_app_ui_state(const char *state, const char *text)
{
    ESP_LOGI(TAG, "apply ui_state state=%s text_len=%u current=%s recording=%d",
             state ? state : "nil",
             (unsigned)(text ? strlen(text) : 0),
             app_ui_state_name(s_app_ui_state),
             s_recording);
    stop_host_response_timer();
    if (strcmp(state, "ready") == 0) {
        if (s_recording) {
            ESP_LOGI(TAG, "ignore ready ui_state while recording");
            return;
        }
        s_app_ui_state = APP_UI_STATE_READY;
        ui_status_set_idle();
        note_activity();
        voice_ble_request_slow_interval();
    } else if (strcmp(state, "recording") == 0) {
        s_app_ui_state = APP_UI_STATE_RECORDING;
        if (!s_recording) {
            ui_status_set_recording(0);
        }
    } else if (strcmp(state, "thinking") == 0) {
        s_app_ui_state = APP_UI_STATE_THINKING;
        ui_status_set_partial_text("");
    } else if (strcmp(state, "pending_confirmation") == 0) {
        s_app_ui_state = APP_UI_STATE_PENDING_CONFIRMATION;
        ui_status_set_partial_text("けっていかキャンセルしてね");
    } else if (strcmp(state, "error") == 0) {
        s_app_ui_state = APP_UI_STATE_ERROR;
        ui_status_set_error(text && text[0] ? text : "よくわからないエラーだよ");
    } else {
        ESP_LOGW(TAG, "unknown ui_state %s", state);
    }
    ESP_LOGI(TAG, "applied ui_state current=%s recording=%d",
             app_ui_state_name(s_app_ui_state),
             s_recording);
}

static void apply_interaction_mode(interaction_mode_t mode)
{
    s_interaction_mode = mode;
    ui_status_set_idle_hint(mode == INTERACTION_MODE_CLICK_TO_TALK ? "ボタンでおはなし"
                                                                   : "おしたままはなそう");
    if (s_app_ui_state == APP_UI_STATE_READY && !s_recording) {
        ui_status_set_idle();
    }
    ESP_LOGI(TAG, "interaction mode %s",
             mode == INTERACTION_MODE_CLICK_TO_TALK ? "click_to_talk" : "hold_to_talk");
}

static void ble_ota_cb(voice_ble_ota_event_t event, uint32_t written, uint32_t size)
{
    switch (event) {
    case VOICE_BLE_OTA_EVENT_BEGIN:
        queue_app_event_with_ota(APP_EVENT_OTA_BEGIN, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_PROGRESS:
        queue_app_event_with_ota(APP_EVENT_OTA_PROGRESS, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_DONE:
        queue_app_event_with_ota(APP_EVENT_OTA_DONE, written, size);
        break;
    case VOICE_BLE_OTA_EVENT_ERROR:
    case VOICE_BLE_OTA_EVENT_ABORT:
        queue_app_event(APP_EVENT_OTA_END);
        break;
    }
}

static void app_event_task(void *arg)
{
    (void)arg;
    app_event_t event;
    while (true) {
        if (xQueueReceive(s_app_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case APP_EVENT_FRONT_DOWN:
            ESP_LOGI(TAG, "button front down");
            note_activity();
            if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK && s_recording) {
                const uint32_t primary_duration_ms = elapsed_button_ms(s_primary_down_us);
                s_primary_session_id = stop_recording();
                esp_err_t primary_up_err = voice_ble_send_button_click("primary", primary_duration_ms,
                                                                       s_primary_session_id);
                if (s_primary_session_id != 0 && primary_up_err != ESP_OK) {
                    ESP_LOGD(TAG, "BLE stop notification skipped/failed: %s",
                             esp_err_to_name(primary_up_err));
                }
                s_primary_down_us = 0;
                s_primary_session_id = 0;
            } else {
                s_primary_down_us = esp_timer_get_time();
                if (s_app_ui_state == APP_UI_STATE_PENDING_CONFIRMATION) {
                    ESP_LOGI(TAG, "button front down as pending confirmation control");
                    s_primary_session_id = 0;
                    (void)voice_ble_send_button_click("primary", 0, 0);
                    break;
                }
                s_primary_session_id = start_recording();
                if (s_primary_session_id == 0) {
                    s_primary_down_us = 0;
                    break;
                }
                esp_err_t primary_down_err = s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK
                    ? voice_ble_send_button_click("primary", 0, s_primary_session_id)
                    : voice_ble_send_button_down("primary", s_primary_session_id);
                if (s_primary_session_id != 0 && primary_down_err != ESP_OK) {
                    ESP_LOGD(TAG, "BLE start notification skipped/failed: %s",
                             esp_err_to_name(primary_down_err));
                }
            }
            break;
        case APP_EVENT_FRONT_UP:
            ESP_LOGI(TAG, "button front up");
            note_activity();
            if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK) {
                break;
            }
            if (!s_recording && s_primary_session_id == 0 && s_primary_down_us == 0) {
                break;
            }
            const uint32_t primary_duration_ms = elapsed_button_ms(s_primary_down_us);
            if (s_recording) {
                s_primary_session_id = stop_recording();
            }
            esp_err_t primary_up_err = voice_ble_send_button_up("primary", primary_duration_ms,
                                                                s_primary_session_id);
            if (s_primary_session_id != 0 && primary_up_err != ESP_OK) {
                ESP_LOGD(TAG, "BLE release notification skipped/failed: %s",
                         esp_err_to_name(primary_up_err));
            }
            s_primary_down_us = 0;
            s_primary_session_id = 0;
            break;
        case APP_EVENT_SIDE_DOWN:
            ESP_LOGI(TAG, "button side down");
            note_activity();
            s_secondary_down_us = esp_timer_get_time();
            break;
        case APP_EVENT_SIDE_UP: {
            ESP_LOGI(TAG, "button side up");
            note_activity();
            const uint32_t secondary_duration_ms = elapsed_button_ms(s_secondary_down_us);
            voice_ble_send_button_click("secondary", secondary_duration_ms, 0);
            s_secondary_down_us = 0;
            if (secondary_duration_ms >= SIDE_LONG_PRESS_MS) {
                show_storage_capacity();
            } else if (s_recording) {
                ESP_LOGW(TAG, "Sync skipped: recording in progress");
                ui_status_set_idle_hint("ろくおん中はおくれないよ");
            } else if (wifi_sync_is_running()) {
                ESP_LOGI(TAG, "Sync already in progress");
            } else {
                ui_status_set_syncing("Wi-Fiをさがしてるよ");
                esp_err_t sync_err = wifi_sync_start();
                if (sync_err != ESP_OK) {
                    ESP_LOGW(TAG, "Sync start failed: %s", esp_err_to_name(sync_err));
                    ui_status_set_idle_hint("いまはおくれないよ");
                }
            }
            break;
        }
        case APP_EVENT_WIFI_SYNC_DONE:
            show_sync_result((wifi_sync_result_t)event.error, event.written, event.size);
            break;
        case APP_EVENT_WIFI_CONNECTED:
            ui_status_set_wifi_connected("Macにおくってるよ");
            break;
        case APP_EVENT_UI_STATE:
            apply_app_ui_state(event.state, event.text);
            break;
        case APP_EVENT_BLE_CONNECTED:
            if (s_recording) {
                s_app_ui_state = APP_UI_STATE_RECORDING;
                ui_status_set_recording(audio_pipeline_session_id());
            } else {
                s_app_ui_state = APP_UI_STATE_READY;
                ui_status_set_idle();
            }
            note_activity();
            break;
        case APP_EVENT_BLE_DISCONNECTED:
            s_ota_updating = false;
            stop_host_response_timer();
            release_ota_pm_locks();
            if (s_recording) {
                s_app_ui_state = APP_UI_STATE_RECORDING;
                ui_status_set_recording(audio_pipeline_session_id());
                ESP_LOGI(TAG, "BLE disconnected; local recording continues");
            } else {
                s_app_ui_state = APP_UI_STATE_READY;
                /* Local-only app: BLE disconnecting just means "no phone
                 * app attached," which is the normal, permanent state here
                 * — show Ready, not Pairing (see the boot-time comment). */
                ui_status_set_idle();
            }
            break;
        case APP_EVENT_AUDIO_ERROR:
            ESP_LOGE(TAG, "local recording failed: %s", esp_err_to_name(event.error));
            s_recording = false;
            s_primary_down_us = 0;
            s_primary_session_id = 0;
            release_recording_pm_locks();
            restart_display_dim_timer();
            restart_deep_sleep_timer();
            s_app_ui_state = APP_UI_STATE_ERROR;
            ui_status_set_error("ろくおんできなかったよ");
            break;
        case APP_EVENT_POWER_IRQ:
            gpio_intr_enable(STICK_S3_PIN_PMIC_IRQ);
            /* fall through */
        case APP_EVENT_BATTERY_REFRESH:
            update_battery_status();
            break;
        case APP_EVENT_ENTER_DEEP_SLEEP:
            enter_deep_sleep();
            break;
        case APP_EVENT_OTA_BEGIN:
            s_ota_updating = true;
            if (s_recording) {
                const uint32_t session_id = stop_recording();
                voice_ble_send_button_up("primary", elapsed_button_ms(s_primary_down_us),
                                         session_id);
            }
            esp_err_t ota_pm_err = acquire_ota_pm_locks();
            if (ota_pm_err != ESP_OK) {
                ESP_LOGW(TAG, "acquire OTA pm lock failed: %s", esp_err_to_name(ota_pm_err));
            }
            note_activity();
            ui_status_set_ota_progress(event.written, event.size);
            break;
        case APP_EVENT_OTA_PROGRESS:
            s_ota_updating = true;
            ui_status_set_ota_progress(event.written, event.size);
            break;
        case APP_EVENT_OTA_DONE:
            ui_status_set_ota_rebooting();
            break;
        case APP_EVENT_OTA_END:
            s_ota_updating = false;
            release_ota_pm_locks();
            s_app_ui_state = APP_UI_STATE_READY;
            stop_host_response_timer();
            ui_status_set_idle();
            note_activity();
            break;
        case APP_EVENT_HOST_RESPONSE_TIMEOUT:
            if (!s_recording && (s_app_ui_state == APP_UI_STATE_RECORDING ||
                                 s_app_ui_state == APP_UI_STATE_THINKING)) {
                ESP_LOGW(TAG, "host response timeout, returning to ready");
                apply_app_ui_state("ready", "");
            }
            break;
        }
    }
}

static esp_err_t init_gpio_button(gpio_num_t gpio_num, button_handle_t *button)
{
    const button_config_t button_config = {0};
    const button_gpio_config_t gpio_config = {
        .gpio_num = gpio_num,
        .active_level = 0,
        .enable_power_save = true
    };

    return iot_button_new_gpio_device(&button_config, &gpio_config, button);
}

static esp_err_t init_buttons(void)
{
    s_app_event_queue = xQueueCreate(12, sizeof(app_event_t));
    if (!s_app_event_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_gpio_button(STICK_S3_PIN_BUTTON_FRONT, &s_front_button);
    if (err != ESP_OK) {
        return err;
    }
    err = init_gpio_button(STICK_S3_PIN_BUTTON_SIDE, &s_side_button);
    if (err != ESP_OK) {
        return err;
    }

    err = iot_button_register_cb(s_front_button, BUTTON_PRESS_DOWN, NULL,
                                 front_button_down_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_front_button, BUTTON_PRESS_UP, NULL,
                                 front_button_up_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_side_button, BUTTON_PRESS_DOWN, NULL,
                                 side_button_down_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = iot_button_register_cb(s_side_button, BUTTON_PRESS_UP, NULL,
                                 side_button_up_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(app_event_task, "app_event_task", 4096,
                                NULL, 6, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void display_dim_timer_cb(void *arg)
{
    (void)arg;

    if (!s_display_dimmed && !s_recording && !s_ota_updating) {
        esp_err_t err = ui_status_set_brightness(DISPLAY_DIM_BRIGHTNESS);
        if (err == ESP_OK) {
            s_display_dimmed = true;
            ui_status_set_idle_dimmed(true);
            ESP_LOGI(TAG, "display dimmed after inactivity");
        } else {
            ESP_LOGW(TAG, "dim display failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t init_display_dim_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = display_dim_timer_cb,
        .name = "display_dim",
    };
    return esp_timer_create(&timer_args, &s_display_dim_timer);
}

static void capacity_display_timer_cb(void *arg)
{
    (void)arg;
    if (!s_recording && !s_ota_updating) {
        ui_status_set_idle();
    }
}

static esp_err_t init_capacity_display_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = capacity_display_timer_cb,
        .name = "capacity_display",
    };
    return esp_timer_create(&timer_args, &s_capacity_display_timer);
}

static void sync_result_display_timer_cb(void *arg)
{
    (void)arg;
    if (!s_recording && !s_ota_updating) {
        ui_status_set_idle();
    }
}

static esp_err_t init_sync_result_display_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = sync_result_display_timer_cb,
        .name = "sync_result_display",
    };
    return esp_timer_create(&timer_args, &s_sync_result_display_timer);
}

static void show_sync_result(wifi_sync_result_t result, unsigned uploaded, unsigned failed)
{
    char hint[64];
    const char *status = "";
    switch (result) {
    case WIFI_SYNC_RESULT_OK:
        if (uploaded > 0) {
            snprintf(hint, sizeof(hint), "%uこおくったよ", uploaded);
        } else {
            strlcpy(hint, "ぜんぶとどいてるよ", sizeof(hint));
        }
        break;
    case WIFI_SYNC_RESULT_PARTIAL_FAIL:
        status = "おしい！";
        snprintf(hint, sizeof(hint), "%uこはあとでね", failed);
        break;
    case WIFI_SYNC_RESULT_NO_NETWORK:
        status = "みつからないよ";
        strlcpy(hint, "またあとでね", sizeof(hint));
        break;
    case WIFI_SYNC_RESULT_ERROR:
    default:
        status = "こまったよ…";
        strlcpy(hint, "Wi-Fiがうごかないよ", sizeof(hint));
        break;
    }
    ESP_LOGI(TAG, "wifi sync result=%d (uploaded=%u failed=%u)", (int)result, uploaded, failed);
    if (result == WIFI_SYNC_RESULT_OK) {
        ui_status_set_sync_success(hint);
    } else {
        ui_status_set_sync_problem(status, hint);
    }
    refresh_storage_label();

    if (s_sync_result_display_timer) {
        (void)esp_timer_stop(s_sync_result_display_timer);
        esp_err_t timer_err = esp_timer_start_once(s_sync_result_display_timer, SYNC_RESULT_DISPLAY_TIMEOUT_US);
        if (timer_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to arm sync result display timer: %s", esp_err_to_name(timer_err));
        }
    }
}

/* Top-row "remaining space" readout; refreshed after anything that changes it. */
static void refresh_storage_label(void)
{
    uint64_t used_bytes = 0;
    uint64_t capacity_bytes = 0;
    if (recording_store_get_usage(&used_bytes, &capacity_bytes) != ESP_OK) {
        return;
    }
    ui_status_set_storage(recording_store_on_sd(),
                          capacity_bytes > used_bytes ? capacity_bytes - used_bytes : 0);
}

static void show_storage_capacity(void)
{
    uint64_t used_bytes = 0;
    uint64_t capacity_bytes = 0;
    esp_err_t err = recording_store_get_usage(&used_bytes, &capacity_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "capacity query failed: %s", esp_err_to_name(err));
        ui_status_set_capacity("わからないよ");
        return;
    }

    char text[64];
    const uint64_t free_bytes = capacity_bytes > used_bytes ? capacity_bytes - used_bytes : 0;
    if (recording_store_on_sd()) {
        const double gb = 1024.0 * 1024.0 * 1024.0;
        snprintf(text, sizeof(text), "SDあと%.1f/%.1fGB",
                 free_bytes / gb, capacity_bytes / gb);
    } else {
        const double mb = 1024.0 * 1024.0;
        snprintf(text, sizeof(text), "あと%.1f/%.1fMB",
                 free_bytes / mb, capacity_bytes / mb);
    }
    ui_status_set_storage(recording_store_on_sd(), free_bytes);
    ESP_LOGI(TAG, "storage capacity: %s", text);
    ui_status_set_capacity(text);

    if (s_capacity_display_timer) {
        (void)esp_timer_stop(s_capacity_display_timer);
        esp_err_t timer_err = esp_timer_start_once(s_capacity_display_timer, CAPACITY_DISPLAY_TIMEOUT_US);
        if (timer_err != ESP_OK) {
            ESP_LOGW(TAG, "start capacity display timer failed: %s", esp_err_to_name(timer_err));
        }
    }
}

static void deep_sleep_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_ENTER_DEEP_SLEEP);
}

static void host_response_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_HOST_RESPONSE_TIMEOUT);
}

static esp_err_t init_deep_sleep_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = deep_sleep_timer_cb,
        .name = "deep_sleep",
    };
    return esp_timer_create(&timer_args, &s_deep_sleep_timer);
}

static esp_err_t init_host_response_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = host_response_timer_cb,
        .name = "host_response",
    };
    return esp_timer_create(&timer_args, &s_host_response_timer);
}

static void battery_refresh_timer_cb(void *arg)
{
    (void)arg;
    queue_app_event(APP_EVENT_BATTERY_REFRESH);
}

static esp_err_t init_battery_refresh_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = battery_refresh_timer_cb,
        .name = "battery_refresh",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_battery_refresh_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_battery_refresh_timer, BATTERY_REFRESH_FALLBACK_US);
}

static void IRAM_ATTR pmic_irq_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(STICK_S3_PIN_PMIC_IRQ);

    BaseType_t high_task_woken = pdFALSE;
    queue_app_event_from_isr(APP_EVENT_POWER_IRQ, &high_task_woken);
    if (high_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t init_pmic_irq(void)
{
    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << STICK_S3_PIN_PMIC_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&irq_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_wakeup_enable(STICK_S3_PIN_PMIC_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(STICK_S3_PIN_PMIC_IRQ, pmic_irq_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_intr_type(STICK_S3_PIN_PMIC_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_intr_enable(STICK_S3_PIN_PMIC_IRQ);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

static void update_battery_status(void)
{
    uint8_t sys_status = 0;
    esp_err_t irq_err = stick_s3_board_clear_power_irqs(&sys_status);
    if (irq_err == ESP_OK && sys_status) {
        ESP_LOGI(TAG, "PMIC sys irq=0x%02x", sys_status);
    }

    int level = 0;
    bool charging = false;
    bool usb_powered = false;
    esp_err_t err = stick_s3_board_battery_level(&level);
    if (err == ESP_OK) {
        err = stick_s3_board_battery_charging(&charging);
    }
    if (err == ESP_OK) {
        err = stick_s3_board_usb_powered(&usb_powered);
    }
    if (err == ESP_OK) {
        const bool external_power_changed = (charging != s_battery_charging) ||
                                            (usb_powered != s_usb_powered);
        s_battery_charging = charging;
        s_usb_powered = usb_powered;
        ui_status_set_battery(level, charging, usb_powered);
        if (external_power_changed) {
            ESP_LOGI(TAG, "power source changed charging=%d usb=%d",
                     charging, usb_powered);
            restart_deep_sleep_timer();
        }
    } else {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot reset_reason=%d wakeup_cause=%d ext1_status=0x%llx",
             esp_reset_reason(), esp_sleep_get_wakeup_cause(),
             (unsigned long long)esp_sleep_get_ext1_wakeup_status());
    ESP_LOGW(TAG, "HEAP diag: free=%u largest_free_block=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    ESP_ERROR_CHECK(init_power_management());
    ESP_ERROR_CHECK(stick_s3_board_init());
    ESP_ERROR_CHECK(ui_status_init());
    ESP_ERROR_CHECK(init_display_dim_timer());
    ESP_ERROR_CHECK(init_capacity_display_timer());
    ESP_ERROR_CHECK(init_sync_result_display_timer());
    ESP_ERROR_CHECK(init_deep_sleep_timer());
    ESP_ERROR_CHECK(init_host_response_timer());
    note_activity();
    voice_ble_set_connection_callback(ble_connection_cb);
    voice_ble_set_control_callback(ble_control_cb);
    voice_ble_set_ota_callback(ble_ota_cb);
    ESP_ERROR_CHECK(init_buttons());

    esp_err_t wifi_sync_err = wifi_sync_init();
    if (wifi_sync_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi Sync init failed; side-button Sync unavailable, local recording unaffected: %s",
                 esp_err_to_name(wifi_sync_err));
    }
    wifi_sync_set_done_callback(wifi_sync_done_cb);
    wifi_sync_set_connected_callback(wifi_sync_connected_cb);

    esp_err_t ble_err = voice_ble_init();
    if (ble_err != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed; local recording remains available: %s",
                 esp_err_to_name(ble_err));
    } else {
        ui_status_set_device_name(voice_ble_device_name());
    }

    esp_err_t audio_err = audio_pipeline_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio_err));
        s_app_ui_state = APP_UI_STATE_ERROR;
        ui_status_set_error("マイクのじゅんびができないよ");
    } else {
        audio_pipeline_set_error_callback(audio_pipeline_error_cb);
        ui_status_set_recording_on_sd(recording_store_on_sd());
        refresh_storage_label();
        apply_interaction_mode(INTERACTION_MODE_CLICK_TO_TALK);
        /* This app is local-only now (no BLE companion app), so BLE never
         * connects in normal use — showing "Pairing" here would mean the
         * screen reads "Pairing" almost permanently. Show "Ready" instead;
         * the BLE radio still advertises in the background regardless. */
        ui_status_set_idle();
    }
    ESP_LOGI(TAG, "Voice Stick booted");

    update_battery_status();
    ESP_ERROR_CHECK(init_battery_refresh_timer());
    ESP_ERROR_CHECK(init_pmic_irq());

    ESP_LOGI(TAG, "configuring PMIC");
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        /* false while bring-up/debugging over USB: automatic light sleep
         * makes USB-Serial-JTAG unresponsive, which breaks esptool's
         * reset-to-bootloader handshake for reflashing. Revert to true
         * for the production power design (see Gate 13 in the project's
         * canonical doc). */
        .light_sleep_enable = false,
    };
    esp_pm_configure(&pm_config);
}
