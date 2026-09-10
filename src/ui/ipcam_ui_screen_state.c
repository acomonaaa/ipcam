#include "ipcam_ui_components.h"

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_WARNING        0xfa7faaU
#define IPCAM_UI_DANGER         0xfa7faaU

/* 创建 P08 工程状态页；异常说明不依赖确认按钮，保证触摸异常时仍可诊断。 */
static int create_state(ipcam_ui_t *ui, ipcam_ui_state_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (!ipcam_ui_make_top_bar(ui, root, "启动及设备异常状态")) return -1;

    if (!ipcam_ui_make_panel(ui, root, 12, 64, 248, 152,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_icon(ui, root, LV_SYMBOL_SETTINGS, 32, 88, 30, 30,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    view->startup_title = ipcam_ui_make_label(ui, root, "启动中", 32, 124, 210, 30,
                                              ipcam_ui_font_body(ui),
                                              lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                              LV_TEXT_ALIGN_LEFT, 0);
    view->startup_text = ipcam_ui_make_label(ui, root, "正在初始化摄像头…", 32, 156,
                                             210, 48, ipcam_ui_font_small(ui),
                                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                             LV_TEXT_ALIGN_LEFT, 1);
    if (!view->startup_title || !view->startup_text) return -1;

    if (!ipcam_ui_make_panel(ui, root, 276, 64, 248, 152,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_icon(ui, root, LV_SYMBOL_WARNING, 296, 88, 30, 30,
                            lv_color_hex(IPCAM_UI_DANGER), 0)) return -1;
    view->camera_title = ipcam_ui_make_label(ui, root, "摄像头不可用", 296, 124, 210, 30,
                                             ipcam_ui_font_body(ui),
                                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                             LV_TEXT_ALIGN_LEFT, 0);
    view->camera_text = ipcam_ui_make_label(ui, root,
                                            "视频区域展示错误说明；设置仍可访问",
                                            296, 156, 210, 48,
                                            ipcam_ui_font_small(ui),
                                            lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                            LV_TEXT_ALIGN_LEFT, 1);
    if (!view->camera_title || !view->camera_text) return -1;

    if (!ipcam_ui_make_panel(ui, root, 540, 64, 248, 152,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_icon(ui, root, LV_SYMBOL_EYE_OPEN, 560, 88, 30, 30,
                            lv_color_hex(IPCAM_UI_WARNING), 0)) return -1;
    view->frame_title = ipcam_ui_make_label(ui, root, "等待画面", 560, 124, 210, 30,
                                            ipcam_ui_font_body(ui),
                                            lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                            LV_TEXT_ALIGN_LEFT, 0);
    view->frame_text = ipcam_ui_make_label(ui, root, "无视频帧时不保留旧画面", 560, 156,
                                           210, 48, ipcam_ui_font_small(ui),
                                           lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                           LV_TEXT_ALIGN_LEFT, 1);
    if (!view->frame_title || !view->frame_text) return -1;

    lv_obj_t *touch_card = ipcam_ui_make_panel(ui, root, 12, 236, 776, 128,
                                               &ui->styles.pressed);
    if (!touch_card) return -1;
    if (!ipcam_ui_make_icon(ui, touch_card, LV_SYMBOL_WARNING, 20, 32, 30, 30,
                            lv_color_hex(IPCAM_UI_WARNING), 0)) return -1;
    view->touch_title = ipcam_ui_make_label(ui, touch_card, "触摸或显示适配异常",
                                            68, 12, 300, 30,
                                            ipcam_ui_font_body(ui),
                                            lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                            LV_TEXT_ALIGN_LEFT, 0);
    view->touch_text = ipcam_ui_make_label(ui, touch_card,
                                           "作为工程诊断状态记录；不依赖无法操作的确认按钮作为唯一恢复方式。",
                                           68, 52, 680, 48,
                                           ipcam_ui_font_small(ui),
                                           lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                           LV_TEXT_ALIGN_LEFT, 1);
    if (!view->touch_title || !view->touch_text) return -1;

    if (!ipcam_ui_make_panel(ui, root, 12, 384, 776, 32,
                             &ui->styles.surface)) return -1;
    view->persistent_text = ipcam_ui_make_label(ui, root,
                                                "长期设备异常保留异常标识，恢复后自动更新",
                                                28, 384, 744, 32,
                                                ipcam_ui_font_small(ui),
                                                lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                LV_TEXT_ALIGN_LEFT, 0);
    return view->persistent_text ? 0 : -1;
}

/* 将启动、摄像头、帧和触摸状态映射为设计要求的可读诊断文案。 */
void ipcam_ui_state_update(ipcam_ui_t *ui, ipcam_ui_state_view_t *view)
{
    if (!ui || !view) return;
    if (ui->state.camera_ready) {
        ipcam_ui_set_label(view->startup_title, "启动完成");
        ipcam_ui_set_label(view->startup_text, "摄像头与显示服务已就绪");
        ipcam_ui_set_label_color(view->startup_title, lv_color_hex(IPCAM_UI_ACCENT));
        ipcam_ui_set_label(view->camera_title, "摄像头正常");
        ipcam_ui_set_label(view->camera_text, "采集能力可用，设置仍可访问");
        ipcam_ui_set_label_color(view->camera_title, lv_color_hex(IPCAM_UI_ACCENT));
    } else {
        ipcam_ui_set_label(view->startup_title, "启动中");
        ipcam_ui_set_label(view->startup_text, "正在初始化摄像头…");
        ipcam_ui_set_label_color(view->startup_title, lv_color_hex(IPCAM_UI_TEXT_PRIMARY));
        ipcam_ui_set_label(view->camera_title, "摄像头不可用");
        ipcam_ui_set_label(view->camera_text, "视频区域展示错误说明；设置仍可访问");
        ipcam_ui_set_label_color(view->camera_title, lv_color_hex(IPCAM_UI_TEXT_PRIMARY));
    }
    if (ui->state.video_frame_valid) {
        ipcam_ui_set_label(view->frame_title, "画面正常");
        ipcam_ui_set_label(view->frame_text, "正在显示最新视频帧");
        ipcam_ui_set_label_color(view->frame_title, lv_color_hex(IPCAM_UI_ACCENT));
    } else {
        ipcam_ui_set_label(view->frame_title, "等待画面");
        ipcam_ui_set_label(view->frame_text, "无视频帧时不保留旧画面");
        ipcam_ui_set_label_color(view->frame_title, lv_color_hex(IPCAM_UI_TEXT_PRIMARY));
    }
    if (ui->state.touch_ready && ui->state.display_ready) {
        ipcam_ui_set_label(view->touch_title, "触摸与显示正常");
        ipcam_ui_set_label(view->touch_text, "触摸坐标已归一化，LVGL 使用单一输入快照");
        ipcam_ui_set_label_color(view->touch_title, lv_color_hex(IPCAM_UI_ACCENT));
    } else {
        ipcam_ui_set_label(view->touch_title, "触摸或显示适配异常");
        ipcam_ui_set_label(view->touch_text,
                           "作为工程诊断状态记录；不依赖无法操作的确认按钮作为唯一恢复方式。");
        ipcam_ui_set_label_color(view->touch_title, lv_color_hex(IPCAM_UI_TEXT_PRIMARY));
    }
}

lv_obj_t *ipcam_ui_state_create(ipcam_ui_t *ui, ipcam_ui_state_view_t *view)
{
    if (!ui || !view) return NULL;
    return create_state(ui, view) == 0 ? view->root : NULL;
}
