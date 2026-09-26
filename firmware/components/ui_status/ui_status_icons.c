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
     * caller picks the scene from recording_store_on_sd(). Keep future Wi-Fi
     * colors tied to explicit application states instead of inferring them. */
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

void ui_status_icons_stop_anim(ui_status_icons_t *icons)
{
    lv_anim_delete(icons->root, NULL);
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
    (void)icons;
    (void)scene;
}
