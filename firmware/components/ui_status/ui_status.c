#include "ui_status.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "stick_s3_board.h"
#include "ui_status_font_jp.h"
#include "ui_status_icons.h"

static const char *TAG = "ui_status";

#define LCD_HOST SPI2_HOST

#define LCD_H_RES 135
#define LCD_V_RES 240
#define LCD_X_GAP 52
#define LCD_Y_GAP 40

#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BACKLIGHT_PWM_HZ 5000
#define LCD_BACKLIGHT_PWM_MAX 255
#define LCD_BACKLIGHT_DEFAULT 128

#define LVGL_DRAW_BUF_LINES 24
#define LVGL_TICK_PERIOD_MS 10
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_TASK_STACK_SIZE (5 * 1024)
#define LVGL_TASK_PRIORITY 2

/* Status (16px, up to 7 full-width characters) sits right under the icon;
 * the hint (12px, may wrap to two lines) sits above the one-line firmware
 * version at the very bottom without touching the status. */
#define UI_STATUS_TOP_Y 156
#define UI_HINT_BOTTOM_Y (-12)
/* Storage text shares the top row with the battery on the right. */
#define UI_STORAGE_LABEL_W 68

#define LCD_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define LCD_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
/* Japanese UTF-8 is 3 bytes per character. */
#define UI_STATUS_TEXT_MAX 48
#define UI_HINT_TEXT_MAX 96

static _lock_t s_lvgl_lock;
static bool s_ready;
static lv_display_t *s_display;
static lv_obj_t *s_screen;
static lv_obj_t *s_storage_label;
static lv_obj_t *s_version_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_battery_shell;
static lv_obj_t *s_battery_fill;
static lv_obj_t *s_battery_tip;
static lv_obj_t *s_battery_label;
static ui_status_icons_t s_icons;
static ui_status_icon_scene_t s_scene = UI_STATUS_ICON_BOOT;
static char s_status_text[UI_STATUS_TEXT_MAX] = "おはよう";
static char s_hint_text[UI_HINT_TEXT_MAX] = "じゅんびちゅう…";
static char s_idle_hint_text[UI_HINT_TEXT_MAX] = "ボタンでおはなし";
static char s_storage_text[40] = "";
static bool s_dimmed;
/* Set once at boot, before any recording starts. */
static bool s_recording_on_sd;

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_display_t *display = (lv_display_t *)user_ctx;
    lv_display_flush_ready(display);
    return false;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    const int x1 = area->x1;
    const int x2 = area->x2;
    const int y1 = area->y1;
    const int y2 = area->y2;

    lv_draw_sw_rgb565_swap(px_map, (x2 - x1 + 1) * (y2 - y1 + 1));
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, px_map);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    while (true) {
        _lock_acquire(&s_lvgl_lock);
        uint32_t delay_ms = lv_timer_handler();
        _lock_release(&s_lvgl_lock);

        delay_ms = MAX(delay_ms, LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(delay_ms * 1000);
    }
}

static void create_battery_ui(lv_obj_t *screen)
{
    s_battery_shell = lv_obj_create(screen);
    lv_obj_remove_style_all(s_battery_shell);
    lv_obj_set_size(s_battery_shell, 20, 10);
    lv_obj_set_style_radius(s_battery_shell, 3, 0);
    lv_obj_set_style_border_width(s_battery_shell, 1, 0);
    lv_obj_set_style_border_color(s_battery_shell, lv_color_hex(0x675f71), 0);
    lv_obj_set_style_bg_opa(s_battery_shell, LV_OPA_TRANSP, 0);
    lv_obj_align(s_battery_shell, LV_ALIGN_TOP_RIGHT, -27, 4);

    s_battery_fill = lv_obj_create(s_battery_shell);
    lv_obj_remove_style_all(s_battery_fill);
    lv_obj_set_size(s_battery_fill, 12, 6);
    lv_obj_set_style_radius(s_battery_fill, 2, 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(0x67c59b), 0);
    lv_obj_align(s_battery_fill, LV_ALIGN_LEFT_MID, 2, 0);

    s_battery_tip = lv_obj_create(screen);
    lv_obj_remove_style_all(s_battery_tip);
    lv_obj_set_size(s_battery_tip, 2, 5);
    lv_obj_set_style_radius(s_battery_tip, 2, 0);
    lv_obj_set_style_bg_opa(s_battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_battery_tip, lv_color_hex(0x675f71), 0);
    lv_obj_align_to(s_battery_tip, s_battery_shell, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    s_battery_label = lv_label_create(screen);
    lv_label_set_text(s_battery_label, "--%");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x675f71), 0);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(s_battery_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_battery_label, 24);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, 0, 4);
}

static void render_scene_locked(ui_status_icon_scene_t scene, const char *status, const char *hint)
{
    if (!s_ready) {
        return;
    }

    ui_status_icons_apply(&s_icons, scene);

    lv_label_set_text(s_status_label, status);
    lv_label_set_text(s_hint_label, hint ? hint : "");
    if (scene == UI_STATUS_ICON_ERROR) {
        lv_obj_set_height(s_hint_label, 36);
        lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_DOT);
        lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, UI_HINT_BOTTOM_Y);
    } else {
        lv_obj_set_height(s_hint_label, LV_SIZE_CONTENT);
        lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
        lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, UI_HINT_BOTTOM_Y);
    }

    const bool resting = scene == UI_STATUS_ICON_RESTING;
    lv_color_t bg = resting ? lv_color_hex(0x1b2430) : lv_color_hex(0xfff7ed);
    lv_color_t text = resting ? lv_color_hex(0xe8eef7) : lv_color_hex(0x3f3440);
    lv_color_t muted = resting ? lv_color_hex(0xa8bad2) : lv_color_hex(0x7f7180);
    lv_color_t hint_color = resting ? lv_color_hex(0xdfe9f8) : muted;

    lv_obj_set_style_bg_color(s_screen, bg, 0);
    lv_obj_set_style_text_color(s_screen, text, 0);
    lv_obj_set_style_text_color(s_storage_label, muted, 0);
    lv_obj_set_style_text_color(s_version_label, muted, 0);
    lv_obj_set_style_text_color(s_status_label, text, 0);
    lv_obj_set_style_text_color(s_hint_label, hint_color, 0);
    lv_obj_set_style_text_color(s_battery_label, muted, 0);
    lv_obj_set_style_border_color(s_battery_shell, muted, 0);
    lv_obj_set_style_bg_color(s_battery_tip, muted, 0);

    ui_status_icons_start_anim(&s_icons, scene);
}

static void render_current_locked(void)
{
    if (s_dimmed) {
        render_scene_locked(UI_STATUS_ICON_RESTING, "すやすや…", "");
    } else {
        render_scene_locked(s_scene, s_status_text, s_hint_text);
    }
}

static void create_status_ui(void)
{
    s_screen = lv_display_get_screen_active(s_display);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xfff7ed), 0);
    lv_obj_set_style_text_color(s_screen, lv_color_hex(0x3f3440), 0);
    lv_obj_set_style_pad_all(s_screen, 8, 0);

    s_storage_label = lv_label_create(s_screen);
    lv_label_set_text(s_storage_label, s_storage_text);
    lv_obj_set_style_text_font(s_storage_label, &ui_font_jp_12, 0);
    lv_obj_set_style_text_color(s_storage_label, lv_color_hex(0x7f7180), 0);
    lv_label_set_long_mode(s_storage_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_storage_label, UI_STORAGE_LABEL_W);
    lv_obj_align(s_storage_label, LV_ALIGN_TOP_LEFT, 0, 1);

    create_battery_ui(s_screen);
    ui_status_icons_create(&s_icons, s_screen);

    s_status_label = lv_label_create(s_screen);
    lv_label_set_text(s_status_label, s_status_text);
    lv_obj_set_style_text_font(s_status_label, &ui_font_jp_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x3f3440), 0);
    lv_obj_set_width(s_status_label, LCD_H_RES - 16);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, UI_STATUS_TOP_Y);

    s_hint_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_hint_label, &ui_font_jp_12, 0);
    lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint_label, LCD_H_RES - 16);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x7f7180), 0);
    lv_label_set_text(s_hint_label, s_hint_text);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, UI_HINT_BOTTOM_Y);

    /* Firmware version + build date so a fresh flash is visible at a glance
     * even when version.txt was not bumped. */
    const esp_app_desc_t *app = esp_app_get_description();
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = 0;
    for (int i = 0; i < 12; ++i) {
        if (strncmp(app->date, months[i], 3) == 0) {
            month = i + 1;
            break;
        }
    }
    const int day = atoi(app->date + 4);
    s_version_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_version_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_version_label, lv_color_hex(0x7f7180), 0);
    lv_label_set_text_fmt(s_version_label, "v%s (%d/%d)", app->version, month, day);
    lv_obj_align(s_version_label, LV_ALIGN_BOTTOM_MID, 0, 2);

    s_ready = true;
    render_current_locked();
}

static void set_scene(ui_status_icon_scene_t scene, const char *status, const char *hint)
{
    _lock_acquire(&s_lvgl_lock);
    s_scene = scene;
    strlcpy(s_status_text, status ? status : "", sizeof(s_status_text));
    strlcpy(s_hint_text, hint ? hint : "", sizeof(s_hint_text));
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

esp_err_t ui_status_init(void)
{
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LCD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = LCD_BACKLIGHT_PWM_HZ,
        // RC_FAST clock remains active during light sleep, preventing backlight flicker
        .clk_cfg = LEDC_USE_RC_FAST_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "configure backlight timer");

    const ledc_channel_config_t backlight_channel = {
        .gpio_num = STICK_S3_PIN_LCD_BL,
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .channel = LCD_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
        .flags.output_invert = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG, "configure backlight channel");

    spi_bus_config_t bus_config = {
        .sclk_io_num = STICK_S3_PIN_LCD_SCK,
        .mosi_io_num = STICK_S3_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "initialize lcd spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = STICK_S3_PIN_LCD_DC,
        .cs_gpio_num = STICK_S3_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &io),
                        TAG, "create lcd panel io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = STICK_S3_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &panel),
                        TAG, "create st7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "invert panel colors");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, false, false), TAG, "mirror panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP), TAG, "set panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "turn display on");

    lv_init();
    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    ESP_RETURN_ON_FALSE(s_display, ESP_ERR_NO_MEM, TAG, "create lvgl display");

    const size_t draw_buffer_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "allocate lvgl draw buffers");

    lv_display_set_buffers(s_display, buf1, buf2, draw_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_display, panel);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, s_display),
                        TAG, "register lcd callbacks");

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG, "create lvgl tick");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000),
                        TAG, "start lvgl tick");

    _lock_acquire(&s_lvgl_lock);
    create_status_ui();
    _lock_release(&s_lvgl_lock);

    BaseType_t task_ok = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE,
                                     NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create lvgl task");

    ESP_RETURN_ON_ERROR(ui_status_set_brightness(LCD_BACKLIGHT_DEFAULT), TAG, "set backlight");
    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}

esp_err_t ui_status_set_brightness(uint8_t brightness)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BACKLIGHT_LEDC_MODE,
                                      LCD_BACKLIGHT_LEDC_CHANNEL,
                                      brightness),
                        TAG, "set backlight duty");
    return ledc_update_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL);
}

void ui_status_prepare_deep_sleep(void)
{
    (void)ui_status_set_brightness(0);

    _lock_acquire(&s_lvgl_lock);
    if (s_display) {
        esp_lcd_panel_handle_t panel = lv_display_get_user_data(s_display);
        if (panel) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel, false));
        }
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_device_name(const char *device_name)
{
    /* The home screen no longer shows the BLE name; keep it in the log only. */
    ESP_LOGD(TAG, "device name %s", device_name ? device_name : "");
}

void ui_status_set_storage(bool on_sd, uint64_t free_bytes)
{
    _lock_acquire(&s_lvgl_lock);
    if (on_sd) {
        snprintf(s_storage_text, sizeof(s_storage_text), "あと%.1fGB",
                 free_bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        /* Internal flash is ~2.5MB; GB would always read 0.0. The MB unit
         * itself shows the SD card is not in use. */
        snprintf(s_storage_text, sizeof(s_storage_text), "あと%.1fMB",
                 free_bytes / (1024.0 * 1024.0));
    }
    if (s_ready) {
        lv_label_set_text(s_storage_label, s_storage_text);
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_advertising(void)
{
    ESP_LOGD(TAG, "advertising");
    set_scene(UI_STATUS_ICON_PAIRING, "なかよし中", "Macのアプリをひらいてね");
}

void ui_status_set_pairing(const char *device_name)
{
    ESP_LOGD(TAG, "pairing %s", device_name ? device_name : "");
    ui_status_set_device_name(device_name);
    set_scene(UI_STATUS_ICON_PAIRING, "なかよし中", device_name ? device_name : "VS-0000");
}

void ui_status_set_idle_hint(const char *hint)
{
    _lock_acquire(&s_lvgl_lock);
    strlcpy(s_idle_hint_text, hint && hint[0] ? hint : "ボタンでおはなし", sizeof(s_idle_hint_text));
    if (s_scene == UI_STATUS_ICON_IDLE) {
        strlcpy(s_hint_text, s_idle_hint_text, sizeof(s_hint_text));
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_idle(void)
{
    ESP_LOGD(TAG, "idle");
    set_scene(UI_STATUS_ICON_IDLE, "まってるよ", s_idle_hint_text);
}

void ui_status_set_idle_dimmed(bool dimmed)
{
    ESP_LOGD(TAG, "idle dimmed=%d", dimmed);
    _lock_acquire(&s_lvgl_lock);
    if (s_dimmed != dimmed) {
        s_dimmed = dimmed;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_recording_on_sd(bool on_sd)
{
    s_recording_on_sd = on_sd;
}

void ui_status_set_recording(uint32_t session_id)
{
    ESP_LOGD(TAG, "recording session %" PRIu32, session_id);
    (void)session_id;

    _lock_acquire(&s_lvgl_lock);
    s_scene = s_recording_on_sd ? UI_STATUS_ICON_RECORDING_SD : UI_STATUS_ICON_RECORDING;
    strlcpy(s_status_text, "きいてるよ♪", sizeof(s_status_text));
    strlcpy(s_hint_text, "ボタンでおわるよ", sizeof(s_hint_text));
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_battery(int level_percent, bool charging, bool usb_powered)
{
    if (level_percent < 0) {
        level_percent = 0;
    } else if (level_percent > 100) {
        level_percent = 100;
    }

    _lock_acquire(&s_lvgl_lock);
    if (s_ready) {
        const int fill_width = MAX(2, (level_percent * 14) / 100);
        lv_obj_set_width(s_battery_fill, fill_width);
        lv_obj_set_style_bg_color(s_battery_fill,
                                  level_percent <= 20 ? lv_color_hex(0xf97373) :
                                  charging || usb_powered ? lv_color_hex(0x5ec4ff) :
                                  lv_color_hex(0x67c59b),
                                  0);
        lv_label_set_text_fmt(s_battery_label, "%d%%", level_percent);
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_partial_text(const char *text)
{
    ESP_LOGD(TAG, "partial: %s", text ? text : "");
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "かんがえ中", text ? text : "");
}

void ui_status_set_capacity(const char *text)
{
    ESP_LOGD(TAG, "capacity: %s", text ? text : "");
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "おなかのぐあい", text ? text : "");
}

void ui_status_set_syncing(const char *hint)
{
    ESP_LOGD(TAG, "sync: %s", hint ? hint : "");
    set_scene(UI_STATUS_ICON_PAIRING, "おでかけ中", hint ? hint : "");
}

void ui_status_set_sync_problem(const char *status, const char *hint)
{
    ESP_LOGD(TAG, "sync problem: %s", hint ? hint : "");
    set_scene(UI_STATUS_ICON_PAIRING, status ? status : "", hint ? hint : "");
}

void ui_status_set_wifi_connected(const char *hint)
{
    set_scene(UI_STATUS_ICON_WIFI, "とどけてるよ", hint ? hint : "");
}

void ui_status_set_sync_success(const char *hint)
{
    ESP_LOGD(TAG, "sync success: %s", hint ? hint : "");
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "とどけたよ！", hint ? hint : "");
}

void ui_status_set_ota_progress(uint32_t written, uint32_t size)
{
    char hint[32];
    uint32_t percent = 0;
    if (size > 0) {
        percent = MIN(100, (written * 100) / size);
    }
    snprintf(hint, sizeof(hint), "%" PRIu32 "%%", percent);
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "おきがえ中", hint);
}

void ui_status_set_ota_rebooting(void)
{
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "できた！", "おきがえしたよ");
}

void ui_status_set_error(const char *message)
{
    ESP_LOGE(TAG, "%s", message ? message : "unknown error");
    set_scene(UI_STATUS_ICON_ERROR, "こまったよ…", message ? message : "よくわからないエラーだよ");
}
