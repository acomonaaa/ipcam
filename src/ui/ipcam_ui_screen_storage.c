#include "ipcam_ui_components.h"

#include <stdio.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_WARNING        0xfa7faaU

/* 创建 P06 存储状态页；容量条是轻量对象，避免引入额外图表控件。 */
static int create_storage(ipcam_ui_t *ui, ipcam_ui_storage_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (!ipcam_ui_make_top_bar(ui, root, "存储状态")) return -1;

    if (!ipcam_ui_make_panel(ui, root, 16, 64, 768, 88,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_icon(ui, root, LV_SYMBOL_SD_CARD, 32, 94, 28, 28,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    if (!ipcam_ui_make_label(ui, root, "SD 卡", 76, 72, 180, 32,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->card_value = ipcam_ui_make_label(ui, root, "已挂载", 76, 112, 260, 28,
                                           ipcam_ui_font_body(ui),
                                           lv_color_hex(IPCAM_UI_ACCENT),
                                           LV_TEXT_ALIGN_LEFT, 0);
    view->card_capacity = ipcam_ui_make_label(ui, root, "容量 32.0 GB", 548, 80,
                                              220, 30, ipcam_ui_font_small(ui),
                                              lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                              LV_TEXT_ALIGN_RIGHT, 0);
    if (!view->card_value || !view->card_capacity) return -1;

    if (!ipcam_ui_make_panel(ui, root, 16, 168, 376, 216,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "空间使用", 32, 184, 200, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->used_value = ipcam_ui_make_label(ui, root, "已使用 0 GB", 32, 228, 200, 28,
                                           ipcam_ui_font_small(ui),
                                           lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                           LV_TEXT_ALIGN_LEFT, 0);
    view->free_value = ipcam_ui_make_label(ui, root, "可用 0 GB", 32, 264, 300, 30,
                                           ipcam_ui_font_body(ui),
                                           lv_color_hex(IPCAM_UI_ACCENT),
                                           LV_TEXT_ALIGN_LEFT, 0);
    if (!view->used_value || !view->free_value) return -1;
    lv_obj_t *usage_track = ipcam_ui_make_panel(ui, root, 32, 308, 328, 20,
                                                &ui->styles.slider_track);
    if (!usage_track) return -1;
    view->usage_fill = ipcam_ui_make_panel(ui, usage_track, 0, 0, 1, 20,
                                           &ui->styles.slider_indicator);
    if (!view->usage_fill) return -1;
    view->usage_percent = ipcam_ui_make_label(ui, root, "已使用 0%", 32, 344, 180, 24,
                                              ipcam_ui_font_small(ui),
                                              lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                              LV_TEXT_ALIGN_LEFT, 0);
    /* 直接保存动态标签句柄，避免依赖根对象子项顺序，后续增加装饰对象时仍能正确刷新。 */
    if (!view->usage_percent) return -1;

    if (!ipcam_ui_make_panel(ui, root, 408, 168, 376, 216,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "录像与设备", 424, 184, 220, 28,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    if (!ipcam_ui_make_label(ui, root, "自动分段", 424, 220, 336, 26,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->device_value = ipcam_ui_make_label(ui, root, "设备：--", 424, 250, 150, 24,
                                             ipcam_ui_font_small(ui),
                                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                             LV_TEXT_ALIGN_LEFT, 0);
    view->filesystem_value = ipcam_ui_make_label(ui, root, "文件系统：--", 424, 278,
                                                 150, 24, ipcam_ui_font_small(ui),
                                                 lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                 LV_TEXT_ALIGN_LEFT, 0);
    view->mount_value = ipcam_ui_make_label(ui, root, "挂载点：--", 424, 306, 336, 24,
                                            ipcam_ui_font_small(ui),
                                            lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                            LV_TEXT_ALIGN_LEFT, 0);
    if (!view->device_value || !view->filesystem_value || !view->mount_value) return -1;
    if (!ipcam_ui_make_label(ui, root, "录像路径随挂载点变化", 424, 334, 150, 24,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->record_state = ipcam_ui_make_label(ui, root, "录像未开始", 424, 358, 144, 24,
                                             ipcam_ui_font_small(ui),
                                             lv_color_hex(IPCAM_UI_ACCENT),
                                             LV_TEXT_ALIGN_LEFT, 0);
    view->record_segment = ipcam_ui_make_label(ui, root, "当前段：—", 424, 374, 144, 20,
                                               ipcam_ui_font_small(ui),
                                               lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                               LV_TEXT_ALIGN_LEFT, 0);
    ipcam_ui_action_t format = { .type = IPCAM_UI_ACTION_STORAGE_FORMAT_REQUEST };
    view->format_button = ipcam_ui_make_button(ui, root, 584, 328, 184, 52,
                                               &ui->styles.danger_button,
                                               &ui->styles.pressed, NULL,
                                               "格式化 SD 卡", &format, NULL, NULL);
    if (!view->record_state || !view->record_segment || !view->format_button) return -1;

    lv_obj_t *warning = ipcam_ui_make_panel(ui, root, 16, 392, 768, 24,
                                            &ui->styles.pressed);
    if (!warning) return -1;
    if (!ipcam_ui_make_icon(ui, warning, LV_SYMBOL_WARNING, 16, 4, 16, 16,
                            lv_color_hex(IPCAM_UI_WARNING), 0)) return -1;
    if (!ipcam_ui_make_label(ui, warning, "未挂载时不会写入根文件系统", 40, 0, 720, 24,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_WARNING),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 0, 416, IPCAM_UI_SCREEN_WIDTH, 64,
                             &ui->styles.surface)) return -1;
    view->format_status = ipcam_ui_make_label(
        ui, root, "存储状态随 SD 卡挂载自动更新", 16, 432, 768, 28,
        ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
        LV_TEXT_ALIGN_LEFT, 0);
    if (!view->format_status) return -1;

    /* 格式化确认框挂在 P06 根对象下，随页面销毁，不会在切换页面后遗留
     * 一个悬空的顶层对象；覆盖层可点击以阻止触摸穿透到后面的按钮。 */
    view->format_overlay = lv_obj_create(root);
    if (!view->format_overlay) return -1;
    lv_obj_set_pos(view->format_overlay, 0, 0);
    lv_obj_set_size(view->format_overlay, IPCAM_UI_SCREEN_WIDTH,
                    IPCAM_UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(view->format_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->format_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(view->format_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(view->format_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(view->format_overlay, 0, 0);
    lv_obj_set_style_pad_all(view->format_overlay, 0, 0);

    view->format_card = ipcam_ui_make_panel(ui, view->format_overlay, 120, 126,
                                            560, 228, &ui->styles.surface);
    if (!view->format_card) return -1;
    if (!ipcam_ui_make_label(ui, view->format_card, "确认格式化 SD 卡", 24, 18,
                             512, 36, ipcam_ui_font_title(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_CENTER, 0)) return -1;
    if (!ipcam_ui_make_label(ui, view->format_card,
                             "格式化将清除 SD 卡中的全部照片和录像，\n确认继续吗？",
                             24, 66, 512, 56, ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_WARNING), LV_TEXT_ALIGN_CENTER, 1))
        return -1;
    ipcam_ui_action_t cancel = { .type = IPCAM_UI_ACTION_STORAGE_FORMAT_CANCEL };
    ipcam_ui_action_t confirm = { .type = IPCAM_UI_ACTION_STORAGE_FORMAT_CONFIRM };
    if (!ipcam_ui_make_button(ui, view->format_card, 24, 148, 220, 56,
                              &ui->styles.pressed, &ui->styles.surface, NULL,
                              "取消", &cancel, NULL, NULL)) return -1;
    if (!ipcam_ui_make_button(ui, view->format_card, 284, 148, 252, 56,
                              &ui->styles.danger_button, &ui->styles.pressed, NULL,
                              "确认格式化", &confirm, NULL, NULL)) return -1;
    lv_obj_add_flag(view->format_overlay, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

/* 刷新容量、设备信息和录像段号；空间不足时保留 warning 视觉层级。 */
void ipcam_ui_storage_update(ipcam_ui_t *ui, ipcam_ui_storage_view_t *view)
{
    if (!ui || !view) return;
    char total[48], available[48], used[48], text[96];
    uint64_t total_bytes = ui->state.storage_total_bytes;
    uint64_t available_bytes = ui->state.storage_available_bytes;
    uint64_t used_bytes = ui->state.storage_used_bytes;
    if (used_bytes == 0 && total_bytes > available_bytes)
        used_bytes = total_bytes - available_bytes;
    ipcam_ui_format_bytes(total_bytes, total, sizeof(total));
    ipcam_ui_format_bytes(available_bytes, available, sizeof(available));
    ipcam_ui_format_bytes(used_bytes, used, sizeof(used));
    snprintf(text, sizeof(text), "%s", ui->state.storage_formatting ? "正在格式化" :
             (ui->state.storage_mounted ? "已挂载" : "未插入"));
    ipcam_ui_set_label(view->card_value, text);
    ipcam_ui_set_label_color(view->card_value,
                             ui->state.storage_mounted ? lv_color_hex(IPCAM_UI_ACCENT) :
                             lv_color_hex(IPCAM_UI_WARNING));
    snprintf(text, sizeof(text), "容量 %s", total_bytes ? total : "--");
    ipcam_ui_set_label(view->card_capacity, text);
    snprintf(text, sizeof(text), "已使用 %s", used);
    ipcam_ui_set_label(view->used_value, text);
    snprintf(text, sizeof(text), "可用 %s", available);
    ipcam_ui_set_label(view->free_value, text);

    unsigned percent = total_bytes ? (unsigned)((used_bytes * 100ULL) / total_bytes) : 0U;
    if (percent > 100U) percent = 100U;
    if (view->usage_fill) {
        int width = (int)((328ULL * percent) / 100ULL);
        if (width < 1) width = 1;
        lv_obj_set_width(view->usage_fill, width);
        if (!ui->state.storage_mounted) lv_obj_add_flag(view->usage_fill, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(view->usage_fill, LV_OBJ_FLAG_HIDDEN);
    }
    snprintf(text, sizeof(text), "已使用 %u%%", percent);
    ipcam_ui_set_label(view->usage_percent, text);
    snprintf(text, sizeof(text), "设备：%s", ui->state.storage_device[0] ?
             ui->state.storage_device : "--");
    ipcam_ui_set_label(view->device_value, text);
    snprintf(text, sizeof(text), "文件系统：%s", ui->state.storage_fs_type[0] ?
             ui->state.storage_fs_type : "--");
    ipcam_ui_set_label(view->filesystem_value, text);
    snprintf(text, sizeof(text), "挂载点：%s", ui->state.storage_mount_path[0] ?
             ui->state.storage_mount_path : "--");
    ipcam_ui_set_label(view->mount_value, text);
    snprintf(text, sizeof(text), "%s", ipcam_ui_record_state_text(ui->state.record_state));
    ipcam_ui_set_label(view->record_state, text);
    if (ui->state.record_segment_no)
        snprintf(text, sizeof(text), "当前段：%u", (unsigned)ui->state.record_segment_no);
    else
        snprintf(text, sizeof(text), "当前段：—");
    ipcam_ui_set_label(view->record_segment, text);
    if (view->format_button) {
        int enabled = ui->state.storage_mounted &&
                      ui->state.storage_format_supported &&
                      !ui->state.storage_formatting;
        if (enabled) lv_obj_remove_state(view->format_button, LV_STATE_DISABLED);
        else lv_obj_add_state(view->format_button, LV_STATE_DISABLED);
    }
    if (view->format_status) {
        const char *status = ui->state.message[0] ? ui->state.message :
                             (ui->state.storage_mounted ?
                              "格式化会清除全部数据，请谨慎操作" :
                              "未挂载时不会写入根文件系统");
        ipcam_ui_set_label(view->format_status, status);
        lv_color_t color = ui->state.message_severity == IPCAM_UI_MESSAGE_ERROR ?
                           lv_color_hex(IPCAM_UI_WARNING) :
                           lv_color_hex(IPCAM_UI_TEXT_SECONDARY);
        ipcam_ui_set_label_color(view->format_status, color);
    }
}

/* 显示或隐藏格式化二次确认框；调用者位于 LVGL 线程。 */
void ipcam_ui_storage_format_prompt(ipcam_ui_t *ui, int visible)
{
    if (!ui || !ui->storage.format_overlay) return;
    if (visible) {
        lv_obj_clear_flag(ui->storage.format_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui->storage.format_overlay);
    } else {
        lv_obj_add_flag(ui->storage.format_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *ipcam_ui_storage_create(ipcam_ui_t *ui, ipcam_ui_storage_view_t *view)
{
    if (!ui || !view) return NULL;
    return create_storage(ui, view) == 0 ? view->root : NULL;
}
