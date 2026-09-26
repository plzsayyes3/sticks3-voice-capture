#include "ui_status_icons.h"

#include <string.h>

#define RADY_ICON_SIZE 112
#define RADY_ICON_STRIDE (RADY_ICON_SIZE * 4)
#define RADY_ICON_DATA_SIZE (RADY_ICON_SIZE * RADY_ICON_STRIDE)
#define RADY_ICON_TOP_Y 42

extern const uint8_t rady_pairing_start[] asm("_binary_rady_pairing_argb8888_bin_start");
extern const uint8_t rady_ready_start[] asm("_binary_rady_ready_argb8888_bin_start");
extern const uint8_t rady_listening_start[] asm("_binary_rady_listening_argb8888_bin_start");
extern const uint8_t rady_listening_sd_start[] asm("_binary_rady_listening_sd_argb8888_bin_start");
extern const uint8_t rady_wifi_start[] asm("_binary_rady_wifi_argb8888_bin_start");
extern const uint8_t rady_thinking_start[] asm("_binary_rady_thinking_argb8888_bin_start");
extern const uint8_t rady_resting_start[] asm("_binary_rady_resting_argb8888_bin_start");
extern const uint8_t rady_error_start[] asm("_binary_rady_error_argb8888_bin_start");

static const lv_image_dsc_t s_rady_pairing = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = RADY_ICON_SIZE,
    .header.h = RADY_ICON_SIZE,
    .header.stride = RADY_ICON_STRIDE,
    .data_size = RADY_ICON_DATA_SIZE,
    .data = rady_pairing_start,
};

static const lv_image_dsc_t s_rady_ready = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = RADY_ICON_SIZE,
    .header.h = RADY_ICON_SIZE,
    .header.stride = RADY_ICON_STRIDE,
    .data_size = RADY_ICON_DATA_SIZE,
    .data = rady_ready_start,
};

static const lv_image_dsc_t s_rady_listening = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = RADY_ICON_SIZE,
    .header.h = RADY_ICON_SIZE,
    .header.stride = RADY_ICON_STRIDE,
    .data_size = RADY_ICON_DATA_SIZE,
    .data = rady_listening_start,
};

static const lv_image_dsc_t s_rady_listening_sd = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = RADY_ICON_SIZE,
    .header.h = RADY_ICON_SIZE,
    .header.stride = RADY_ICON_STRIDE,
    .data_size = RADY_ICON_DATA_SIZE,
    .data = rady_listening_sd_start,
};

static const lv_image_dsc_t s_rady_wifi = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = RADY_ICON_SIZE,
    .header.h = RADY_ICON_SIZE,
    .header.stride = RADY_ICON_STRIDE,
    .data_size = RADY_ICON_DATA_SIZE,
    .data = rady_wifi_start,
};

static const lv_image_dsc_t s_rady_thinking = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = RADY_ICON_SIZE,
    .header.h = RADY_ICON_SIZE,
    .header.stride = RADY_ICON_STRIDE,
    .data_size = RADY_ICON_DATA_SIZE,
    .data = rady_thinking_start,
};

static const lv_image_dsc_t s_rady_resting = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = RADY_ICON_SIZE,
    .header.h = RADY_ICON_SIZE,
    .header.stride = RADY_ICON_STRIDE,
    .data_size = RADY_ICON_DATA_SIZE,
    .data = rady_resting_start,
};

static const lv_image_dsc_t s_rady_error = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = RADY_ICON_SIZE,
    .header.h = RADY_ICON_SIZE,
    .header.stride = RADY_ICON_STRIDE,
    .data_size = RADY_ICON_DATA_SIZE,
    .data = rady_error_start,
};

static const lv_image_dsc_t *get_scene_image(ui_status_icon_scene_t scene)
{
    /* Recording is green for internal flash and pink for the SD card; the
     * caller picks the scene from recording_store_on_sd(). Sky blue is only
     * shown once wifi_sync reports a joined network, never while scanning. */
    switch (scene) {
    case UI_STATUS_ICON_BOOT:
    case UI_STATUS_ICON_PAIRING:
        return &s_rady_pairing;
    case UI_STATUS_ICON_IDLE:
        return &s_rady_ready;
    case UI_STATUS_ICON_RESTING:
        return &s_rady_resting;
    case UI_STATUS_ICON_RECORDING:
        return &s_rady_listening;
    case UI_STATUS_ICON_RECORDING_SD:
        return &s_rady_listening_sd;
    case UI_STATUS_ICON_WIFI:
        return &s_rady_wifi;
    case UI_STATUS_ICON_TRANSCRIBING:
        return &s_rady_thinking;
    case UI_STATUS_ICON_ERROR:
        return &s_rady_error;
    }
    return &s_rady_ready;
}

void ui_status_icons_create(ui_status_icons_t *icons, lv_obj_t *screen)
{
    memset(icons, 0, sizeof(*icons));

    icons->root = lv_image_create(screen);
    lv_obj_remove_style_all(icons->root);
    lv_image_set_src(icons->root, &s_rady_pairing);
    lv_obj_align(icons->root, LV_ALIGN_TOP_MID, 0, RADY_ICON_TOP_Y);
}

/* Last applied bob offset. The exec callback only touches the style (and so
 * only invalidates/redraws the icon) when the whole-pixel offset changes:
 * a 2-3px bob redraws a handful of times per cycle instead of every frame,
 * which matters during hour-long recordings. */
static int32_t s_bob_offset;

static void bob_exec(void *var, int32_t value)
{
    if (value != s_bob_offset) {
        s_bob_offset = value;
        lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
    }
}

void ui_status_icons_stop_anim(ui_status_icons_t *icons)
{
    lv_anim_delete(icons->root, NULL);
    s_bob_offset = 0;
    lv_obj_set_style_translate_y(icons->root, 0, 0);
}

void ui_status_icons_apply(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    ui_status_icons_stop_anim(icons);
    lv_image_set_src(icons->root, get_scene_image(scene));
    lv_obj_set_style_opa(icons->root, LV_OPA_COVER, 0);
    lv_obj_align(icons->root, LV_ALIGN_TOP_MID, 0, RADY_ICON_TOP_Y);
}

void ui_status_icons_start_anim(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    /* Rady "breathes" while waiting, bounces while listening and hops while
     * sending. Other scenes (and the dimmed resting screen) stay still. */
    int32_t amplitude = 0;
    uint32_t half_period_ms = 0;
    switch (scene) {
    case UI_STATUS_ICON_IDLE:
        amplitude = 3;
        half_period_ms = 1600;
        break;
    case UI_STATUS_ICON_RECORDING:
    case UI_STATUS_ICON_RECORDING_SD:
        amplitude = 3;
        half_period_ms = 450;
        break;
    case UI_STATUS_ICON_WIFI:
        amplitude = 2;
        half_period_ms = 700;
        break;
    default:
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, icons->root);
    lv_anim_set_exec_cb(&anim, bob_exec);
    lv_anim_set_values(&anim, 0, -amplitude);
    lv_anim_set_duration(&anim, half_period_ms);
    lv_anim_set_playback_duration(&anim, half_period_ms);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}
