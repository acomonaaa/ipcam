#include "ipcam_ui_components.h"

#include <stdio.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_WARNING        0xfa7faaU
#define IPCAM_UI_SURFACE        0x150f23U
#define IPCAM_UI_BG             0x1f1633U

/* 创建 P03 全屏预览；动态帧由后续 framebuffer/LVGL port 注入，当前仅保留占位。 */
static int create_fullscreen(ipcam_ui_t *ui, ipcam_ui_fullscreen_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 1);
    if (!root) return -1;
    view->root = root;
    if (!ipcam_ui_make_panel(ui, root, 24, 68, 976, 500,
                             &ui->styles.surface)) return -1;
    view->video_placeholder = ipcam_ui_make_label(ui, root, "动态视频占位",
                                                  352, 282, 320, 36,
                                                  ipcam_ui_font_body(ui),
                                                  lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                  LV_TEXT_ALIGN_CENTER, 0);
    if (!view->video_placeholder) return -1;

    ipcam_ui_action_t back = { .type = IPCAM_UI_ACTION_BACK };
    if (!ipcam_ui_make_button(ui, root, 24, 20, 160, 48,
                              &ui->styles.surface, &ui->styles.pressed,
                              LV_SYMBOL_LEFT, "退出全屏", &back, NULL, NULL)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 452, 84, 120, 48,
                             &ui->styles.screen)) return -1;
    view->zoom_value = ipcam_ui_make_label(ui, root, "2.3×", 452, 84, 120, 48,
                                           ipcam_ui_font_body(ui),
                                           lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                           LV_TEXT_ALIGN_CENTER, 0);
    if (!view->zoom_value) return -1;

    view->recording_pill = ipcam_ui_make_panel(ui, root, 824, 20, 176, 48,
                                               &ui->styles.surface);
    if (!view->recording_pill) return -1;
    if (!ipcam_ui_make_panel(ui, view->recording_pill, 16, 16, 16, 16,
                             &ui->styles.danger_button)) return -1;
    view->recording_text = ipcam_ui_make_label(ui, view->recording_pill,
                                               "录像 00:12:08", 40, 0, 120, 48,
                                               ipcam_ui_font_small(ui),
                                               lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                               LV_TEXT_ALIGN_LEFT, 0);
    if (!view->recording_text) return -1;

    ipcam_ui_action_t reset = { .type = IPCAM_UI_ACTION_RESET_VIEW };
    if (!ipcam_ui_make_button(ui, root, 840, 520, 160, 48,
                              &ui->styles.surface, &ui->styles.pressed,
                              LV_SYMBOL_REFRESH, "复位画面", &reset, NULL, NULL)) return -1;
    if (!ipcam_ui_make_label(ui, root,
                             "双指张开放大 · 捏合缩小 · 双指移动改变观察位置",
                             260, 566, 504, 28, ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_CENTER, 0)) return -1;
    return 0;
}

/* 刷新全屏画面上的倍率、录像计时和动态帧状态，不改变控件层级。 */
void ipcam_ui_fullscreen_update(ipcam_ui_t *ui,
                                ipcam_ui_fullscreen_view_t *view)
{
    if (!ui || !view) return;
    char text[96];
    snprintf(text, sizeof(text), "%.1f×", (double)(ui->state.zoom > 0.0f ?
                                                    ui->state.zoom : 1.0f));
    ipcam_ui_set_label(view->zoom_value, text);
    ipcam_ui_format_duration(ui->state.record_elapsed_ms, text, sizeof(text));
    char recording[112];
    snprintf(recording, sizeof(recording), "录像 %s", text);
    ipcam_ui_set_label(view->recording_text, recording);
    if (view->recording_pill) {
        if (ipcam_ui_record_active(ui->state.record_state))
            lv_obj_clear_flag(view->recording_pill, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(view->recording_pill, LV_OBJ_FLAG_HIDDEN);
    }
    if (!ui->state.camera_ready)
        ipcam_ui_set_label(view->video_placeholder, "摄像头不可用");
    else if (ui->state.video_frame_valid)
        ipcam_ui_set_label(view->video_placeholder, "动态视频");
    else
        ipcam_ui_set_label(view->video_placeholder, "动态视频占位");
}

/* 页面构造入口；根对象由 ipcam_ui_destroy 统一释放。 */
lv_obj_t *ipcam_ui_fullscreen_create(ipcam_ui_t *ui,
                                     ipcam_ui_fullscreen_view_t *view)
{
    if (!ui || !view) return NULL;
    return create_fullscreen(ui, view) == 0 ? view->root : NULL;
}
