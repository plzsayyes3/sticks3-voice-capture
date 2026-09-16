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
