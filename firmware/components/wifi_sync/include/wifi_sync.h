#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* One-time setup: NVS, netif, event loop, Wi-Fi driver in STA mode.
 * Safe to call once during app_main, before any wifi_sync_start(). */
esp_err_t wifi_sync_init(void);

/* Scans for the known networks in secrets.h, connects to the first one in
 * range, uploads every completed (*.ogg, not *.part) recording under
 * recording_store_base_path() to STICKS3_RECEIVER_URL, then disconnects.
 * Runs in its own task and returns immediately; safe to call from a button
 * ISR-adjacent event handler. Returns ESP_ERR_INVALID_STATE if a sync is
 * already running or a recording is currently in progress. */
esp_err_t wifi_sync_start(void);

bool wifi_sync_is_running(void);

typedef enum {
    WIFI_SYNC_RESULT_OK,           /* connected, ran sync, nothing failed */
    WIFI_SYNC_RESULT_PARTIAL_FAIL, /* connected, but at least one upload failed */
    WIFI_SYNC_RESULT_NO_NETWORK,   /* no known network in range; never connected */
    WIFI_SYNC_RESULT_ERROR,        /* Wi-Fi driver itself failed to init/start */
} wifi_sync_result_t;

typedef void (*wifi_sync_done_cb_t)(wifi_sync_result_t result, unsigned uploaded, unsigned failed);

/* Called once from wifi_sync_task right before it exits, on every code
 * path (success, partial failure, no network, driver error) — lets the
 * caller (main.c) show a result on screen instead of the sync finishing
 * silently. Invoked from the wifi_sync task's own context, not the
 * caller's, so the callback must only do things safe to call from another
 * task (e.g. queue an event), not touch caller-local state directly. */
void wifi_sync_set_done_callback(wifi_sync_done_cb_t callback);

typedef void (*wifi_sync_connected_cb_t)(void);
/* Called from the wifi_sync task once a known network is joined, before
 * uploads start. Never called when no network is reached. Same threading
 * rule as the done callback: only queue an event. */
void wifi_sync_set_connected_callback(wifi_sync_connected_cb_t callback);
