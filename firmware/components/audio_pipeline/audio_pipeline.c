#include "audio_pipeline.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "opus.h"

#include "recording_store.h"
#include "stick_s3_board.h"

static const char *TAG = "audio_pipeline";

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS 1
#define AUDIO_FRAME_MS 60
#define AUDIO_FRAME_SAMPLES ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)
#define OPUS_BITRATE 20000
#define OPUS_MAX_PACKET_SIZE 220
#define OPUS_COMPLEXITY 1

#define WRITE_QUEUE_DEPTH 50
#define TASK_EXIT_WAIT_MS 5000
#define WRITER_POLL_MS 100

typedef struct {
    uint32_t seq;
    uint16_t len;
    uint8_t data[OPUS_MAX_PACKET_SIZE];
} audio_packet_t;

static atomic_bool s_running;
static atomic_bool s_failed;
static atomic_int s_last_error;
static bool s_initialized;
static uint32_t s_session_id;
static uint32_t s_seq;
static TaskHandle_t s_audio_task;
static TaskHandle_t s_writer_task;
static QueueHandle_t s_write_queue;
static audio_pipeline_error_callback_t s_error_callback;

/* Per-session resources: created on start, destroyed after writer finalization. */
static i2s_chan_handle_t s_rx_handle;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static OpusEncoder *s_opus_encoder;

static bool tasks_exited(void)
{
    return s_audio_task == NULL && s_writer_task == NULL;
}

static esp_err_t wait_for_tasks_to_exit(TickType_t timeout_ticks)
{
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    while (!tasks_exited()) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGE(TAG, "tasks did not exit: audio=%p writer=%p",
                     s_audio_task, s_writer_task);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static void report_failure(esp_err_t error, const char *reason)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_failed, &expected, true)) {
        return;
    }

    atomic_store(&s_last_error, (int)error);
    atomic_store(&s_running, false);
    ESP_LOGE(TAG, "recording failure: %s (%s)", reason, esp_err_to_name(error));

    if (s_error_callback) {
        s_error_callback(error);
    }
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle),
                        TAG, "create i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = STICK_S3_PIN_ES8311_MCLK,
            .bclk = STICK_S3_PIN_ES8311_BCLK,
            .ws = STICK_S3_PIN_ES8311_LRCK,
            .dout = STICK_S3_PIN_ES8311_DIN,
            .din = STICK_S3_PIN_ES8311_DOUT,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg),
                        TAG, "init i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable i2s rx");
    return ESP_OK;
}

static esp_err_t init_codec(void)
{
    i2c_master_bus_handle_t i2c_bus = stick_s3_board_i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "i2c bus unavailable");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create codec i2c ctrl");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_1,
        .rx_handle = s_rx_handle,
        .tx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create codec i2s data");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create codec gpio");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG,
                        "create codec dev");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 1,
        .channel_mask = I2S_STD_SLOT_LEFT,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, 36.0) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set mic gain");
    return ESP_OK;
}

static esp_err_t init_opus(void)
{
    int error = OPUS_OK;
    s_opus_encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                                         OPUS_APPLICATION_VOIP, &error);
    ESP_RETURN_ON_FALSE(s_opus_encoder != NULL && error == OPUS_OK,
                        ESP_FAIL, TAG, "create opus encoder error=%d", error);

    opus_encoder_ctl(s_opus_encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    return ESP_OK;
}

static void deinit_opus(void)
{
    if (s_opus_encoder) {
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
    }
}

static void deinit_codec(void)
{
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
}

static void deinit_i2s(void)
{
    if (s_rx_handle) {
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
}

static void deinit_session_resources(void)
{
    deinit_opus();
    deinit_codec();
    deinit_i2s();
    ESP_LOGI(TAG, "session resources released");
}

static void audio_task(void *arg)
{
    (void)arg;

    int16_t mono[AUDIO_FRAME_SAMPLES];
    uint8_t opus_buf[OPUS_MAX_PACKET_SIZE];
    uint32_t encoded_packets = 0;

    while (atomic_load(&s_running)) {
        esp_err_t err = esp_codec_dev_read(s_codec, mono, sizeof(mono));
        if (err != ESP_OK) {
            report_failure(err, "codec read failed");
            break;
        }

        opus_int32 encoded = opus_encode(s_opus_encoder,
                                         mono,
                                         AUDIO_FRAME_SAMPLES,
                                         opus_buf,
                                         sizeof(opus_buf));
        if (encoded < 0) {
            report_failure(ESP_FAIL, "Opus encode failed");
            break;
        }

        audio_packet_t packet = {
            .seq = s_seq,
            .len = (uint16_t)encoded,
        };
        memcpy(packet.data, opus_buf, (size_t)encoded);

        if (xQueueSend(s_write_queue, &packet, 0) != pdTRUE) {
            report_failure(ESP_ERR_TIMEOUT,
                           "Flash writer queue full; refusing silent packet loss");
            break;
        }

        s_seq++;
        encoded_packets++;
    }

    ESP_LOGI(TAG, "audio task exit: encoded=%" PRIu32, encoded_packets);
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static void writer_task(void *arg)
{
    (void)arg;

    audio_packet_t packet;
    uint32_t written_packets = 0;

    while (true) {
        if (atomic_load(&s_failed)) {
            break;
        }

        if (xQueueReceive(s_write_queue, &packet,
                          pdMS_TO_TICKS(WRITER_POLL_MS)) == pdTRUE) {
            esp_err_t err = recording_store_write_opus(packet.data,
                                                       packet.len,
                                                       AUDIO_FRAME_SAMPLES);
            if (err != ESP_OK) {
                report_failure(err, "Flash/Ogg write failed");
                break;
            }
            written_packets++;
            continue;
        }

        if (!atomic_load(&s_running) &&
            s_audio_task == NULL &&
            uxQueueMessagesWaiting(s_write_queue) == 0) {
            break;
        }
    }

    if (atomic_load(&s_failed)) {
        xQueueReset(s_write_queue);
        recording_store_abort(true);
    } else {
        long final_size = 0;
        esp_err_t err = recording_store_finish(&final_size);
        if (err != ESP_OK) {
            report_failure(err, "recording finalization failed");
        } else {
            ESP_LOGI(TAG, "recording saved: packets=%" PRIu32 " size=%ld",
                     written_packets, final_size);
        }
    }

    while (s_audio_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    deinit_session_resources();
    s_writer_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_pipeline_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(recording_store_init(), TAG, "recording store init");

    s_write_queue = xQueueCreate(WRITE_QUEUE_DEPTH, sizeof(audio_packet_t));
    ESP_RETURN_ON_FALSE(s_write_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "create write queue");

    atomic_store(&s_running, false);
    atomic_store(&s_failed, false);
    atomic_store(&s_last_error, ESP_OK);
    s_initialized = true;
    ESP_LOGI(TAG, "audio pipeline ready for local recording");
    return ESP_OK;
}

void audio_pipeline_set_error_callback(audio_pipeline_error_callback_t callback)
{
    s_error_callback = callback;
}

esp_err_t audio_pipeline_start(uint32_t session_id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "not initialized");
    ESP_RETURN_ON_FALSE(!atomic_load(&s_running), ESP_ERR_INVALID_STATE, TAG,
                        "already recording");
    ESP_RETURN_ON_ERROR(wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS)),
                        TAG, "wait previous session exit");

    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s init");

    esp_err_t err = init_codec();
    if (err != ESP_OK) {
        deinit_i2s();
        ESP_LOGE(TAG, "codec init: %s", esp_err_to_name(err));
        return err;
    }

    err = init_opus();
    if (err != ESP_OK) {
        deinit_codec();
        deinit_i2s();
        ESP_LOGE(TAG, "opus init: %s", esp_err_to_name(err));
        return err;
    }

    opus_int32 lookahead = 0;
    if (opus_encoder_ctl(s_opus_encoder, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK) {
        lookahead = 0;
    }
    uint32_t scaled_pre_skip = ((uint32_t)lookahead * 48000U) / AUDIO_SAMPLE_RATE;
    if (scaled_pre_skip > UINT16_MAX) {
        scaled_pre_skip = UINT16_MAX;
    }

    err = recording_store_begin(session_id,
                                AUDIO_SAMPLE_RATE,
                                (uint16_t)scaled_pre_skip);
    if (err != ESP_OK) {
        deinit_session_resources();
        ESP_LOGE(TAG, "open recording: %s", esp_err_to_name(err));
        return err;
    }

    xQueueReset(s_write_queue);
    s_session_id = session_id;
    s_seq = 0;
    opus_encoder_ctl(s_opus_encoder, OPUS_RESET_STATE);
    atomic_store(&s_failed, false);
    atomic_store(&s_last_error, ESP_OK);
    atomic_store(&s_running, true);

    /* Stacks must stay in internal RAM, not PSRAM: writer_task calls into
     * the flash driver (fwrite/fflush/fsync on the FAT-on-flash partition),
     * which disables the cache across both cores while it runs. ESP-IDF
     * asserts that no running task's own stack lives in PSRAM at that
     * point (esp_task_stack_is_sane_cache_disabled), since PSRAM is
     * unreachable with the cache off — a task stack there would crash
     * mid-context-switch. Confirmed by a real panic/reboot on-device
     * (assert failed in spi_flash_disable_interrupts_caches_and_other_cpu)
     * when this was tried. */
    /* 28672 rather than a round 32768: by the time BLE/Wi-Fi/netif init
     * has run, internal RAM is fragmented enough that the largest single
     * contiguous block reliably available is ~31.7KB (see HEAP diag log),
     * capped by a fixed 32KB region CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL
     * reserves for PSRAM DMA bounce buffers. 32768 never fits; this stays
     * safely under that ceiling with headroom. */
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task,
                                            "audio_capture",
                                            28672,
                                            NULL,
                                            5,
                                            &s_audio_task,
                                            1);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        s_audio_task = NULL;
        recording_store_abort(false);
        deinit_session_resources();
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreatePinnedToCore(writer_task,
                                 "record_writer",
                                 6144,
                                 NULL,
                                 6,
                                 &s_writer_task,
                                 0);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        (void)wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
        xQueueReset(s_write_queue);
        recording_store_abort(false);
        deinit_session_resources();
        s_writer_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "start local recording session=%" PRIu32 " path=%s",
             session_id, recording_store_current_path());
    return ESP_OK;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!tasks_exited()) {
        atomic_store(&s_running, false);
        ESP_LOGI(TAG, "stop session %" PRIu32 "; draining writer", s_session_id);
        esp_err_t wait_err = wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
        if (wait_err != ESP_OK) {
            return wait_err;
        }
    }

    return (esp_err_t)atomic_load(&s_last_error);
}

esp_err_t audio_pipeline_last_error(void)
{
    return (esp_err_t)atomic_load(&s_last_error);
}
