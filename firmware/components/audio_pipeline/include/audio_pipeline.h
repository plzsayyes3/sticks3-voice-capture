#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef void (*audio_pipeline_error_callback_t)(esp_err_t error);

esp_err_t audio_pipeline_init(void);
void audio_pipeline_set_error_callback(audio_pipeline_error_callback_t callback);
esp_err_t audio_pipeline_start(uint32_t session_id);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_pipeline_last_error(void);
uint32_t audio_pipeline_session_id(void);
