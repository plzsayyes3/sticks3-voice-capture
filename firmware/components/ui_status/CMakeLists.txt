idf_component_register(
    SRCS "ui_status.c" "ui_status_icons.c"
         "fonts/ui_font_jp_12.c" "fonts/ui_font_jp_16.c"
    INCLUDE_DIRS "include" "."
    EMBED_FILES
        "assets/rady_pairing_argb8888.bin"
        "assets/rady_ready_argb8888.bin"
        "assets/rady_listening_argb8888.bin"
        "assets/rady_listening_sd_argb8888.bin"
        "assets/rady_wifi_argb8888.bin"
        "assets/rady_thinking_argb8888.bin"
        "assets/rady_resting_argb8888.bin"
        "assets/rady_error_argb8888.bin"
    REQUIRES driver esp_driver_ledc esp_lcd esp_timer esp_app_format stick_s3_board lvgl
)
