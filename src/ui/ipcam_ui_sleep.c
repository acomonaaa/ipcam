/* 休眠提示覆盖层；它只创建 LVGL 控件，不直接访问背光或计时线程。 */
#define IPCAM_LOG_MODULE "UI  "
#include "ipcam_log.h"
#include "ipcam_ui_components.h"

#include <stdio.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_SURFACE        0x150f23U
#define IPCAM_UI_INK            0x101418U

/*
 * 创建全局休眠提示。
 * 覆盖层挂到 lv_layer_top()，因此 P01～P08 都能复用同一组按钮；背景设为
 * 可点击是为了阻止提示期间的触摸穿透，只有两个明确按钮可以改变策略。
 */
int ipcam_ui_sleep_prompt_create(ipcam_ui_t *ui)
{
    if (!ui) return -1;
    ui->sleep_overlay = lv_obj_create(lv_layer_top());
    if (!ui->sleep_overlay) {
        MLOGE("create sleep prompt overlay failed\n");
        return -1;
    }
    lv_obj_set_pos(ui->sleep_overlay, 0, 0);
    lv_obj_set_size(ui->sleep_overlay, IPCAM_UI_SCREEN_WIDTH,
                    IPCAM_UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(ui->sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->sleep_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ui->sleep_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->sleep_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(ui->sleep_overlay, 0, 0);
    lv_obj_set_style_pad_all(ui->sleep_overlay, 0, 0);

    /*
     * 快速路径的两个 image 必须先于卡片创建，保证它们位于提示卡片之下。
     * 对象本身是可选的：即使后续固定缓冲分配失败，旧的半透明提示仍需可用。
     */
    ui->sleep_backdrop = lv_image_create(ui->sleep_overlay);
    ui->sleep_video_image = lv_image_create(ui->sleep_overlay);
    if (ui->sleep_backdrop) {
        lv_obj_set_pos(ui->sleep_backdrop, 0, 0);
        lv_obj_set_size(ui->sleep_backdrop, IPCAM_UI_SCREEN_WIDTH,
                        IPCAM_UI_SCREEN_HEIGHT);
        lv_obj_add_flag(ui->sleep_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui->sleep_video_image) {
        lv_obj_set_pos(ui->sleep_video_image, 0, 0);
        lv_obj_set_size(ui->sleep_video_image, IPCAM_UI_SCREEN_WIDTH,
                        IPCAM_UI_SCREEN_HEIGHT);
        lv_obj_add_flag(ui->sleep_video_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (!ui->sleep_backdrop || !ui->sleep_video_image) {
        MLOGW("sleep fast overlay objects unavailable; keep alpha fallback\n");
        if (ui->sleep_backdrop) lv_obj_del(ui->sleep_backdrop);
        if (ui->sleep_video_image) lv_obj_del(ui->sleep_video_image);
        ui->sleep_backdrop = NULL;
        ui->sleep_video_image = NULL;
    }

    ui->sleep_card = ipcam_ui_make_panel(ui, ui->sleep_overlay, 140, 130,
                                         520, 220, &ui->styles.surface);
    if (!ui->sleep_card) return -1;
    lv_obj_set_style_bg_color(ui->sleep_card, lv_color_hex(IPCAM_UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(ui->sleep_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui->sleep_card, 1, 0);
    lv_obj_set_style_border_color(ui->sleep_card, lv_color_hex(IPCAM_UI_ACCENT), 0);
    lv_obj_set_style_radius(ui->sleep_card, 12, 0);

    if (!ipcam_ui_make_label(ui, ui->sleep_card, "屏幕休眠提示", 24, 18,
                             472, 36, ipcam_ui_font_title(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_CENTER, 0)) return -1;
    ui->sleep_countdown = ipcam_ui_make_label(
        ui, ui->sleep_card, "屏幕将在 10 秒后休眠", 24, 62, 472, 40,
        ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
        LV_TEXT_ALIGN_CENTER, 0);
    if (!ui->sleep_countdown) return -1;

    ipcam_ui_action_t keep = { .type = IPCAM_UI_ACTION_SCREEN_KEEP_AWAKE };
    ipcam_ui_action_t sleep = { .type = IPCAM_UI_ACTION_SCREEN_SLEEP };
    ui->sleep_keep_button = ipcam_ui_make_button(
        ui, ui->sleep_card, 24, 132, 220, 56, &ui->styles.accent_button,
        &ui->styles.pressed, NULL, "继续显示", &keep, NULL, NULL);
    ui->sleep_sleep_button = ipcam_ui_make_button(
        ui, ui->sleep_card, 276, 132, 220, 56, &ui->styles.danger_button,
        &ui->styles.pressed, NULL, "立即休眠", &sleep, NULL, NULL);
    if (!ui->sleep_keep_button || !ui->sleep_sleep_button) return -1;

    lv_obj_add_flag(ui->sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

/* 根据 screen 服务快照显示倒计时；不在每秒重建对象，减少小 CPU 上的分配抖动。 */
void ipcam_ui_sleep_prompt_update(ipcam_ui_t *ui)
{
    if (!ui || !ui->sleep_overlay) return;
    if (!ui->state.screen_sleep_prompt && !ui->state.screen_sleeping) {
        lv_obj_add_flag(ui->sleep_overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (ui->state.screen_sleeping || !ui->state.screen_sleep_prompt) {
        lv_obj_add_flag(ui->sleep_overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int remaining = ui->state.screen_sleep_remaining_sec;
    if (remaining < 1) remaining = 1;
    char text[64];
    snprintf(text, sizeof(text), "屏幕将在 %d 秒后休眠", remaining);
    ipcam_ui_set_label(ui->sleep_countdown, text);
    lv_obj_clear_flag(ui->sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->sleep_overlay);
}
