#ifndef IPCAM_UI_STYLES_H
#define IPCAM_UI_STYLES_H

#include <lvgl.h>

#include "ipcam_ui.h"

/* 设计文件中复用的颜色和文本样式，集中维护以避免页面发生视觉漂移。 */
typedef struct ipcam_ui_styles_s {
    lv_style_t screen;
    lv_style_t black_screen;
    lv_style_t surface;
    lv_style_t pressed;
    lv_style_t accent_button;
    lv_style_t danger_button;
    lv_style_t label_primary;
    lv_style_t label_secondary;
    lv_style_t label_accent;
    lv_style_t label_success;
    lv_style_t label_warning;
    lv_style_t row;
    lv_style_t row_selected;
    lv_style_t slider_track;
    lv_style_t slider_indicator;
    lv_style_t slider_knob;
    lv_style_t toggle_track;
    lv_style_t toggle_active;
    lv_style_t toggle_knob;
} ipcam_ui_styles_t;

void ipcam_ui_styles_init(ipcam_ui_styles_t *styles,
                          const ipcam_ui_fonts_t *fonts);

#endif /* IPCAM_UI_STYLES_H */
