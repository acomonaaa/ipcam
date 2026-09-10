#include "ipcam_ui_components.h"

#include <stdio.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_WARNING        0xfa7faaU

static int make_video_radio(ipcam_ui_t *ui, lv_obj_t *root,
                            int x, int y, const char *text,
                            const ipcam_ui_action_t *action,
                            lv_obj_t **row, lv_obj_t **dot, lv_obj_t **label)
{
    return ipcam_ui_make_radio_row(ui, root, x, y, 216, 48, text, action,
                                   row, dot, label);
}

/* P04 以三列设置卡片承载视频参数，所有选项命中区都保持 48px 高。 */
static int create_video_settings(ipcam_ui_t *ui, ipcam_ui_video_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (!ipcam_ui_make_top_bar(ui, root, "视频设置")) return -1;

    view->lock_notice = ipcam_ui_make_panel(ui, root, 12, 56, 776, 48,
                                            &ui->styles.pressed);
    if (!view->lock_notice) return -1;
    if (!ipcam_ui_make_icon(ui, view->lock_notice, LV_SYMBOL_WARNING,
                            16, 14, 20, 20, lv_color_hex(IPCAM_UI_WARNING), 0))
        return -1;
    if (!ipcam_ui_make_label(ui, view->lock_notice,
                             "录像中锁定视频参数，请先停止录像", 44, 0, 716, 48,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_WARNING),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 12, 116, 248, 264,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "分辨率", 28, 128, 212, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    ipcam_ui_action_t res640 = { .type = IPCAM_UI_ACTION_SELECT_VIDEO_PRESET,
                                 .value0 = 640, .value1 = 480 };
    ipcam_ui_action_t res800 = { .type = IPCAM_UI_ACTION_SELECT_VIDEO_PRESET,
                                 .value0 = 800, .value1 = 600 };
    ipcam_ui_action_t res1280 = { .type = IPCAM_UI_ACTION_SELECT_VIDEO_PRESET,
                                  .value0 = 1280, .value1 = 720 };
    if (make_video_radio(ui, root, 28, 164, "640×480", &res640,
                         &view->resolution_rows[0], &view->resolution_dots[0],
                         &view->resolution_labels[0]) != 0) return -1;
    if (make_video_radio(ui, root, 28, 216, "800×600", &res800,
                         &view->resolution_rows[1], &view->resolution_dots[1],
                         &view->resolution_labels[1]) != 0) return -1;
    if (make_video_radio(ui, root, 28, 268, "1280×720", &res1280,
                         &view->resolution_rows[2], &view->resolution_dots[2],
                         &view->resolution_labels[2]) != 0) return -1;
    if (!ipcam_ui_make_label(ui, root, "按摄像头能力启用", 28, 324, 216, 32,
                             ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 276, 116, 248, 264,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "帧率", 292, 128, 212, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    ipcam_ui_action_t fps15 = { .type = IPCAM_UI_ACTION_SELECT_VIDEO_PRESET,
                                .value2 = 15 };
    ipcam_ui_action_t fps10 = { .type = IPCAM_UI_ACTION_SELECT_VIDEO_PRESET,
                                .value2 = 10 };
    ipcam_ui_action_t fps5 = { .type = IPCAM_UI_ACTION_SELECT_VIDEO_PRESET,
                               .value2 = 5 };
    if (make_video_radio(ui, root, 292, 164, "15 fps", &fps15,
                         &view->fps_rows[0], &view->fps_dots[0],
                         &view->fps_labels[0]) != 0) return -1;
    if (make_video_radio(ui, root, 292, 216, "10 fps", &fps10,
                         &view->fps_rows[1], &view->fps_dots[1],
                         &view->fps_labels[1]) != 0) return -1;
    if (make_video_radio(ui, root, 292, 268, "5 fps", &fps5,
                         &view->fps_rows[2], &view->fps_dots[2],
                         &view->fps_labels[2]) != 0) return -1;
    if (!ipcam_ui_make_label(ui, root, "低帧率降低存储占用", 292, 324, 216, 32,
                             ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 540, 116, 248, 124,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "JPEG 质量", 556, 128, 180, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    ipcam_ui_action_t jpeg_medium = { .type = IPCAM_UI_ACTION_SELECT_JPEG_QUALITY,
                                      .value0 = 75 };
    ipcam_ui_action_t jpeg_high = { .type = IPCAM_UI_ACTION_SELECT_JPEG_QUALITY,
                                    .value0 = 90 };
    if (ipcam_ui_make_radio_row(ui, root, 556, 160, 104, 48, "中",
                                &jpeg_medium, &view->jpeg_rows[0],
                                &view->jpeg_dots[0], &view->jpeg_labels[0]) != 0)
        return -1;
    if (ipcam_ui_make_radio_row(ui, root, 672, 160, 104, 48, "高",
                                &jpeg_high, &view->jpeg_rows[1],
                                &view->jpeg_dots[1], &view->jpeg_labels[1]) != 0)
        return -1;

    if (!ipcam_ui_make_panel(ui, root, 540, 252, 248, 164,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "画面翻转", 556, 264, 180, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    ipcam_ui_action_t mirror_h = { .type = IPCAM_UI_ACTION_TOGGLE_MIRROR_HORIZONTAL };
    ipcam_ui_action_t mirror_v = { .type = IPCAM_UI_ACTION_TOGGLE_MIRROR_VERTICAL };
    view->mirror_horizontal_toggle = ipcam_ui_make_toggle(
        ui, root, 556, 304, 132, 48, &mirror_h,
        &view->mirror_horizontal_knob);
    if (!view->mirror_horizontal_toggle) return -1;
    if (!ipcam_ui_make_label(ui, root, "水平镜像", 700, 304, 76, 48,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->mirror_vertical_toggle = ipcam_ui_make_toggle(
        ui, root, 556, 360, 132, 48, &mirror_v,
        &view->mirror_vertical_knob);
    if (!view->mirror_vertical_toggle) return -1;
    if (!ipcam_ui_make_label(ui, root, "垂直翻转", 700, 360, 76, 48,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    if (!ipcam_ui_make_label(ui, root, "修改后点击应用；录像中修改会暂存", 12, 380,
                             516, 32, ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    if (!ipcam_ui_make_panel(ui, root, 0, 416, IPCAM_UI_SCREEN_WIDTH, 64,
                             &ui->styles.surface)) return -1;
    ipcam_ui_action_t cancel = { .type = IPCAM_UI_ACTION_BACK };
    ipcam_ui_action_t apply = { .type = IPCAM_UI_ACTION_APPLY_VIDEO };
    if (!ipcam_ui_make_button(ui, root, 548, 420, 112, 52,
                              &ui->styles.pressed, &ui->styles.surface,
                              NULL, "取消", &cancel, NULL, NULL)) return -1;
    if (!ipcam_ui_make_button(ui, root, 672, 420, 112, 52,
                              &ui->styles.accent_button, &ui->styles.pressed,
                              NULL, "应用", &apply, NULL, NULL)) return -1;
    return 0;
}

/* 更新 pending 选中态；应用前不覆盖用户选择，避免状态轮询抹掉修改。 */
void ipcam_ui_video_update(ipcam_ui_t *ui, ipcam_ui_video_view_t *view)
{
    if (!ui || !view) return;
    ipcam_ui_video_pending_t pending = ui->video_pending;
    if (!ui->video_pending_valid) {
        pending.width = ui->state.video_width;
        pending.height = ui->state.video_height;
        pending.fps = ui->state.target_fps;
        pending.jpeg_quality = ui->state.jpeg_quality;
        pending.mirror_horizontal = ui->state.mirror_horizontal;
        pending.mirror_vertical = ui->state.mirror_vertical;
    }
    const int widths[3] = { 640, 800, 1280 };
    const int heights[3] = { 480, 600, 720 };
    const int fps[3] = { 15, 10, 5 };
    for (int i = 0; i < 3; i++) {
        ipcam_ui_set_radio_selected(view->resolution_rows[i],
                                    view->resolution_dots[i],
                                    view->resolution_labels[i],
                                    pending.width == widths[i] && pending.height == heights[i]);
        ipcam_ui_set_radio_selected(view->fps_rows[i], view->fps_dots[i],
                                    view->fps_labels[i], pending.fps == fps[i]);
    }
    for (int i = 0; i < 2; i++)
        ipcam_ui_set_radio_selected(view->jpeg_rows[i], view->jpeg_dots[i],
                                    view->jpeg_labels[i],
                                    (i == 0 && pending.jpeg_quality < 90) ||
                                    (i == 1 && pending.jpeg_quality >= 90));
    ipcam_ui_toggle_set(view->mirror_horizontal_toggle,
                        view->mirror_horizontal_knob,
                        pending.mirror_horizontal, &ui->styles);
    ipcam_ui_toggle_set(view->mirror_vertical_toggle,
                        view->mirror_vertical_knob,
                        pending.mirror_vertical, &ui->styles);
    if (view->lock_notice) {
        if (ipcam_ui_record_active(ui->state.record_state))
            lv_obj_clear_flag(view->lock_notice, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(view->lock_notice, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *ipcam_ui_video_create(ipcam_ui_t *ui, ipcam_ui_video_view_t *view)
{
    if (!ui || !view) return NULL;
    return create_video_settings(ui, view) == 0 ? view->root : NULL;
}
