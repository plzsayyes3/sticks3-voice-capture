#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_random.h"

esp_err_t recording_store_init(void);
const char *recording_store_base_path(void);
esp_err_t recording_store_begin(uint32_t session_id,
                                uint32_t input_sample_rate,
                                uint16_t pre_skip_48k);
esp_err_t recording_store_write_opus(const uint8_t *packet,
                                     size_t packet_len,
                                     uint32_t frame_samples);
esp_err_t recording_store_finish(long *out_size);
void recording_store_abort(bool keep_partial);
bool recording_store_active(void);
const char *recording_store_current_path(void);
