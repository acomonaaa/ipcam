#include "ipcam_ui_components.h"

#include <stdio.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU

/* 创建 P05 屏幕设置；滑块对象外扩到 48px 高以满足触摸命中要求。 */
static int create_screen_settings(ipcam_ui_t *ui, ipcam_ui_screen_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (!ipcam_ui_make_top_bar(ui, root, "屏幕设置")) return -1;

    if (!ipcam_ui_make_panel(ui, root, 16, 64, 768, 120,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "亮度", 32, 78, 200, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->brightness_value = ipcam_ui_make_label(ui, root, "70%", 680, 78, 72, 28,
                                                 ipcam_ui_font_body(ui),
                                                 lv_color_hex(IPCAM_UI_ACCENT),
                                                 LV_TEXT_ALIGN_RIGHT, 0);
    if (!view->brightness_value) return -1;

    view->brightness_slider = lv_slider_create(root);
    if (!view->brightness_slider) return -1;
    lv_obj_set_pos(view->brightness_slider, 32, 104);
    lv_obj_set_size(view->brightness_slider, 704, 48);
    lv_slider_set_range(view->brightness_slider, 10, 100);
    lv_slider_set_value(view->brightness_slider, 70, LV_ANIM_OFF);
    lv_obj_add_style(view->brightness_slider, &ui->styles.slider_track, LV_PART_MAIN);
    lv_obj_add_style(view->brightness_slider, &ui->styles.slider_indicator,
                     LV_PART_INDICATOR);
    lv_obj_add_style(view->brightness_slider, &ui->styles.slider_knob, LV_PART_KNOB);
    lv_obj_set_style_height(view->brightness_slider, 16, LV_PART_MAIN);
    lv_obj_set_style_height(view->brightness_slider, 16, LV_PART_INDICATOR);
    lv_obj_set_style_width(view->brightness_slider, 48, LV_PART_KNOB);
    lv_obj_set_style_height(view->brightness_slider, 48, LV_PART_KNOB);
    ipcam_ui_action_t brightness = { .type = IPCAM_UI_ACTION_SET_BACKLIGHT,
                                     .value0 = 70 };
    /* VALUE_CHANGED 能覆盖点击滑轨、拖动滑块和无完整 release 报告的触摸驱动；
     * 上层动作队列会合并连续亮度值，避免拖动时阻塞媒体线程。 */
    if (ipcam_ui_bind_action(ui, view->brightness_slider, LV_EVENT_VALUE_CHANGED,
                             &brightness) != 0) return -1;
    if (!ipcam_ui_make_label(ui, root, "10%", 32, 146, 80, 24,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    if (!ipcam_ui_make_label(ui, root, "100%", 656, 146, 80, 24,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_RIGHT, 0)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 16, 200, 376, 208,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "自动熄屏", 32, 212, 220, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    const int timeout_values[5] = { 0, 1, 3, 5, 10 };
    const char *timeout_text[5] = { "永不", "1 分钟", "3 分钟", "5 分钟", "10 分钟" };
    const int timeout_x[5] = { 32, 208, 32, 208, 32 };
    const int timeout_y[5] = { 252, 252, 304, 304, 356 };
    for (int i = 0; i < 5; i++) {
        ipcam_ui_action_t action = { .type = IPCAM_UI_ACTION_SET_SCREEN_TIMEOUT,
                                     .value0 = timeout_values[i] };
        if (ipcam_ui_make_radio_row(ui, root, timeout_x[i], timeout_y[i], 160, 48,
                                    timeout_text[i], &action,
                                    &view->timeout_rows[i], &view->timeout_dots[i],
                                    &view->timeout_labels[i]) != 0) return -1;
    }

    lv_obj_t *zoom_card = ipcam_ui_make_panel(ui, root, 408, 200, 376, 120,
                                              &ui->styles.pressed);
    if (!zoom_card) return -1;
    if (!ipcam_ui_make_icon(ui, zoom_card, LV_SYMBOL_EYE_OPEN, 20, 24, 26, 26,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    if (!ipcam_ui_make_label(ui, zoom_card, "预览缩放", 56, 12, 260, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    if (!ipcam_ui_make_label(ui, zoom_card, "双指手势可在预览页调整倍率", 56, 52,
                             300, 28, ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    lv_obj_t *footer = ipcam_ui_make_panel(ui, root, 408, 336, 376, 72,
                                           &ui->styles.surface);
    if (!footer) return -1;
    if (!ipcam_ui_make_label(ui, footer, "熄屏只暂停本地预览，不影响录像和电脑观看", 16, 8,
                             344, 56, ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 1)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 0, 416, IPCAM_UI_SCREEN_WIDTH, 64,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "设置会自动保存", 16, 432, 768, 28,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_ACCENT),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    return 0;
}

/* 更新滑块、百分比和熄屏单选态；不在状态回放中重新触发控制动作。 */
void ipcam_ui_screen_settings_update(ipcam_ui_t *ui,
                                     ipcam_ui_screen_view_t *view)
{
    if (!ui || !view) return;
    int brightness = ui->state.backlight_percent;
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    if (view->brightness_slider)
        lv_slider_set_value(view->brightness_slider, brightness, LV_ANIM_OFF);
    char text[32];
    snprintf(text, sizeof(text), "%d%%", brightness);
    ipcam_ui_set_label(view->brightness_value, text);
    const int timeout_values[5] = { 0, 1, 3, 5, 10 };
    for (int i = 0; i < 5; i++)
        ipcam_ui_set_radio_selected(view->timeout_rows[i], view->timeout_dots[i],
                                    view->timeout_labels[i],
                                    ui->state.screen_timeout_min == timeout_values[i]);
}

lv_obj_t *ipcam_ui_screen_settings_create(ipcam_ui_t *ui,
                                          ipcam_ui_screen_view_t *view)
{
    if (!ui || !view) return NULL;
    return create_screen_settings(ui, view) == 0 ? view->root : NULL;
}
