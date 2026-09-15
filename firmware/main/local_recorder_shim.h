#pragma once

/*
 * Transitional compatibility layer for the VoiceStick-derived app shell.
 *
 * The existing main.c was designed around a live BLE host: it refuses to start
 * when BLE is not ready, treats button-notification failures as recording
 * failures, and stops recording on disconnect. Local recording must not inherit
 * those requirements. This header is force-included only for main.c so the
 * VoiceStick UI/OTA shell can remain intact while audio_pipeline becomes an
 * offline-first recorder.
 *
 * Remove this shim once main.c is refactored around explicit local-recorder
 * states and side-button Sync.
 */

#include "audio_pipeline.h"
#include "voice_ble.h"

static voice_ble_connection_cb_t s_local_recorder_app_connection_cb;
static voice_ble_control_cb_t s_local_recorder_app_control_cb;

static void local_recorder_connection_proxy(bool connected)
{
    if (connected && s_local_recorder_app_connection_cb) {
        s_local_recorder_app_connection_cb(true);
    }
    /*
     * Deliberately suppress disconnect events. The imported app shell handles
     * APP_EVENT_BLE_DISCONNECTED by stopping audio_pipeline(), which is invalid
     * for an offline recorder. Sync state will get its own event model later.
     */
}

static inline void local_recorder_set_connection_callback(
    voice_ble_connection_cb_t callback)
{
    s_local_recorder_app_connection_cb = callback;
    voice_ble_set_connection_callback(local_recorder_connection_proxy);
}

static inline void local_recorder_set_control_callback(
    voice_ble_control_cb_t callback)
{
    s_local_recorder_app_control_cb = callback;
    voice_ble_set_control_callback(callback);
}

static void local_recorder_audio_error(esp_err_t error)
{
    (void)error;
    if (s_local_recorder_app_control_cb) {
        s_local_recorder_app_control_cb(
            "{\"event\":\"ui_state\",\"state\":\"error\",\"text\":\"Recording failed\"}");
    }
}

static inline esp_err_t local_recorder_audio_init(void)
{
    esp_err_t err = audio_pipeline_init();
    if (err == ESP_OK) {
        audio_pipeline_set_error_callback(local_recorder_audio_error);
    }
    return err;
}

static inline esp_err_t local_recorder_ble_init(void)
{
    esp_err_t err = voice_ble_init();
    if (err == ESP_OK && s_local_recorder_app_control_cb) {
        /*
         * The imported VoiceStick defaults to hold-to-talk and normally lets a
         * BLE host switch interaction modes. Offline capture has no host, so
         * force the target UX: one front-button click starts, the next stops.
         */
        s_local_recorder_app_control_cb(
            "{\"event\":\"interaction_mode\",\"mode\":\"click_to_talk\"}");
    }
    return err;
}

static inline bool local_recorder_ble_ready(void)
{
    /* Recording availability is a property of local storage, not BLE. */
    return true;
}

static inline esp_err_t local_recorder_button_down(const char *button,
                                                   uint32_t session_id)
{
    if (voice_ble_is_connected()) {
        (void)voice_ble_send_button_down(button, session_id);
    }
    /* A host notification must never veto local capture. */
    return ESP_OK;
}

static inline esp_err_t local_recorder_button_up(const char *button,
                                                 uint32_t duration_ms,
                                                 uint32_t session_id)
{
    if (voice_ble_is_connected()) {
        (void)voice_ble_send_button_up(button, duration_ms, session_id);
    }
    /*
     * main.c uses a non-OK result after stop to return the imported host-driven
     * UI to READY. Recording has already been finalized locally at this point.
     */
    return ESP_FAIL;
}

static inline esp_err_t local_recorder_button_click(const char *button,
                                                    uint32_t duration_ms,
                                                    uint32_t session_id)
{
    if (voice_ble_is_connected()) {
        (void)voice_ble_send_button_click(button, duration_ms, session_id);
    }

    /* click-to-talk starts with duration=0 and must not be cancelled. */
    if (session_id != 0 && duration_ms == 0) {
        return ESP_OK;
    }

    /* Stopping click-to-talk should return the legacy UI to READY locally. */
    return ESP_FAIL;
}

/*
 * Keep these macros after the wrappers so calls inside the wrappers resolve to
 * the original VoiceStick symbols rather than recursively expanding.
 */
#define voice_ble_set_connection_callback local_recorder_set_connection_callback
#define voice_ble_set_control_callback local_recorder_set_control_callback
#define voice_ble_init local_recorder_ble_init
#define voice_ble_is_ready local_recorder_ble_ready
#define voice_ble_send_button_down local_recorder_button_down
#define voice_ble_send_button_up local_recorder_button_up
#define voice_ble_send_button_click local_recorder_button_click
#define audio_pipeline_init local_recorder_audio_init
