#include "ipcam_ui_components.h"

#include <stdio.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_WARNING        0xfa7faaU
#define IPCAM_UI_PRESSED        0x3f3849U
#define IPCAM_UI_INK            0x101418U

/*
 * P01/P02 共用顶部状态栏。状态栏坐标来自最新 .pen，而不是沿用旧的
 * 1024×600 导出值；这样切换分辨率后网络胶囊和 SD 状态不会越界重叠。
 */
static int make_home_header(ipcam_ui_t *ui, lv_obj_t *root,
                            ipcam_ui_home_view_t *view)
{
    lv_obj_t *bar = ipcam_ui_make_panel(ui, root, 0, 0,
                                        IPCAM_UI_SCREEN_WIDTH, 48,
                                        &ui->styles.surface);
    if (!bar) return -1;

    if (!ipcam_ui_make_label(ui, bar, "IPCam", 16, 0, 160, 48,
                             ipcam_ui_font_title(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    lv_obj_t *network = ipcam_ui_make_panel(ui, bar, 176, 8, 300, 32,
                                            &ui->styles.pressed);
    if (!network) return -1;
    if (!ipcam_ui_make_icon(ui, network, LV_SYMBOL_WIFI, 12, 8, 16, 16,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    view->network_text = ipcam_ui_make_label(ui, network,
                                             "有线已连接 · 192.168.1.100",
                                             36, 0, 252, 32,
                                             ipcam_ui_font_small(ui),
                                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                             LV_TEXT_ALIGN_LEFT, 0);
    if (!view->network_text) return -1;

    /* 顶部 SD 状态也必须是完整命中区；用户不需要先找到下方卡片才能查看
     * 容量、设备和文件系统信息。 */
    ipcam_ui_action_t storage = { .type = IPCAM_UI_ACTION_OPEN_STORAGE };
    lv_obj_t *storage_card = ipcam_ui_make_action_panel(
        ui, bar, 596, 4, 204, 40, &ui->styles.pressed,
        &ui->styles.surface, &storage);
    if (!storage_card) return -1;
    if (!ipcam_ui_make_icon(ui, storage_card, LV_SYMBOL_SD_CARD, 12, 12, 18, 18,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    view->storage_text = ipcam_ui_make_label(ui, storage_card, "SD 卡 28.4 GB 可用",
                                             36, 4, 160, 32,
                                             ipcam_ui_font_small(ui),
                                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                             LV_TEXT_ALIGN_LEFT, 0);
    return view->storage_text ? 0 : -1;
}

/*
 * 创建设置卡片骨架。子控件仍使用设计文件的绝对坐标换算到卡片局部坐标，
 * 这样卡片既能承载点击事件，也不会因父对象改变而丢失 .pen 的像素关系。
 */
static lv_obj_t *make_home_card(ipcam_ui_t *ui, lv_obj_t *root,
                                int x, int y, int width, int height,
                                int icon_x, int icon_y, int icon_w, int icon_h,
                                int title_x, int title_y, int title_w, int title_h,
                                int value_x, int value_y, int value_w, int value_h,
                                const char *icon, const char *title,
                                const char *value, const ipcam_ui_action_t *action,
                                lv_obj_t **value_out)
{
    lv_obj_t *card = ipcam_ui_make_action_panel(ui, root, x, y, width, height,
                                                &ui->styles.surface,
                                                &ui->styles.pressed, action);
    if (!card) return NULL;
    if (!ipcam_ui_make_icon(ui, card, icon, icon_x - x, icon_y - y,
                            icon_w, icon_h, lv_color_hex(IPCAM_UI_ACCENT), 0))
        return NULL;
    if (!ipcam_ui_make_label(ui, card, title, title_x - x, title_y - y,
                             title_w, title_h, ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return NULL;
    lv_obj_t *value_obj = ipcam_ui_make_label(ui, card, value,
                                              value_x - x, value_y - y,
                                              value_w, value_h,
                                              ipcam_ui_font_small(ui),
                                              lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                              LV_TEXT_ALIGN_LEFT, 1);
    if (value_out) *value_out = value_obj;
    return value_obj ? card : NULL;
}

/* 视频窗口上的图标只有图形没有底板；按钮本身保留 48×48 命中区。 */
static lv_obj_t *make_video_icon_button(ipcam_ui_t *ui, lv_obj_t *root,
                                        int x, int y, const char *symbol,
                                        const ipcam_ui_action_t *action)
{
    lv_obj_t *button = lv_button_create(root);
    if (!button) return NULL;
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 48, 48);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(button, LV_OPA_0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    if (action && ipcam_ui_bind_action(ui, button, LV_EVENT_CLICKED, action) != 0) {
        lv_obj_del(button);
        return NULL;
    }
    return ipcam_ui_make_icon(ui, button, symbol, 4, 4, 40, 40,
                              lv_color_hex(IPCAM_UI_TEXT_PRIMARY), 0) ? button : NULL;
}

/* 创建用于视频帧注入的 LVGL image；descriptor 由 ipcam_lvgl 服务稳定持有。 */
static int make_home_video_image(ipcam_ui_t *ui, ipcam_ui_home_view_t *view,
                                 lv_obj_t *root)
{
    view->video_surface = ipcam_ui_make_panel(ui, root, 24, 104, 476, 268,
                                              &ui->styles.black_screen);
    if (!view->video_surface) return -1;
    view->video_image = lv_image_create(view->video_surface);
    if (!view->video_image) return -1;
    lv_obj_set_pos(view->video_image, 0, 0);
    lv_obj_set_size(view->video_image, 476, 268);
    /* display 已按 476×268 目标尺寸生成 RGB565；禁止 STRETCH 触发每帧软件缩放。 */
    lv_image_set_inner_align(view->video_image, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_add_flag(view->video_image, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

/* 创建 P01 本地预览首页：视频区 500×336，设置区 260×336，底栏 72px。 */
static int create_local_home(ipcam_ui_t *ui, ipcam_ui_home_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (make_home_header(ui, root, view) != 0) return -1;

    if (!ipcam_ui_make_panel(ui, root, 12, 60, 500, 336,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "本地预览", 24, 72, 240, 28,
                             ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->fps_text = ipcam_ui_make_label(ui, root, "实测 14.8 fps", 320, 72,
                                         176, 28, ipcam_ui_font_small(ui),
                                         lv_color_hex(IPCAM_UI_ACCENT),
                                         LV_TEXT_ALIGN_RIGHT, 0);
    if (!view->fps_text || make_home_video_image(ui, view, root) != 0) return -1;

    view->video_placeholder = ipcam_ui_make_label(ui, view->video_surface,
                                                  "动态视频占位", 98, 112,
                                                  280, 56,
                                                  ipcam_ui_font_body(ui),
                                                  lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                  LV_TEXT_ALIGN_CENTER, 1);
    if (!view->video_placeholder) return -1;
    view->video_resolution = ipcam_ui_make_label(ui, root, "640×480", 36, 344,
                                                 140, 24,
                                                 ipcam_ui_font_small(ui),
                                                 lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                 LV_TEXT_ALIGN_LEFT, 0);
    view->zoom_text = ipcam_ui_make_label(ui, root, "1.0×", 400, 344, 80, 24,
                                          ipcam_ui_font_small(ui),
                                          lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                          LV_TEXT_ALIGN_RIGHT, 0);
    if (!view->video_resolution || !view->zoom_text) return -1;

    ipcam_ui_action_t fullscreen = { .type = IPCAM_UI_ACTION_SHOW_FULLSCREEN };
    ipcam_ui_action_t reset = { .type = IPCAM_UI_ACTION_RESET_VIEW };
    if (!make_video_icon_button(ui, root, 408, 108, LV_SYMBOL_EYE_OPEN,
                                &fullscreen)) return -1;
    if (!make_video_icon_button(ui, root, 456, 108, LV_SYMBOL_REFRESH,
                                &reset)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 528, 60, 260, 336,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "观看模式", 540, 72, 100, 24,
                             ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    ipcam_ui_action_t local_mode = { .type = IPCAM_UI_ACTION_SHOW_LOCAL };
    ipcam_ui_action_t computer_mode = { .type = IPCAM_UI_ACTION_SHOW_COMPUTER };
    view->mode_local = ipcam_ui_make_button(ui, root, 540, 100, 112, 48,
                                            &ui->styles.accent_button,
                                            &ui->styles.pressed, NULL, "本地＋电脑",
                                            &local_mode, NULL,
                                            &view->mode_local_label);
    view->mode_computer = ipcam_ui_make_button(ui, root, 660, 100, 112, 48,
                                               &ui->styles.pressed,
                                               &ui->styles.surface, NULL, "电脑",
                                               &computer_mode, NULL,
                                               &view->mode_computer_label);
    if (!view->mode_local || !view->mode_computer) return -1;

    ipcam_ui_action_t light = { .type = IPCAM_UI_ACTION_SET_LIGHT_TOGGLE };
    ipcam_ui_action_t video = { .type = IPCAM_UI_ACTION_OPEN_VIDEO_SETTINGS };
    ipcam_ui_action_t screen = { .type = IPCAM_UI_ACTION_OPEN_SCREEN_SETTINGS };
    ipcam_ui_action_t storage = { .type = IPCAM_UI_ACTION_OPEN_STORAGE };
    ipcam_ui_action_t network = { .type = IPCAM_UI_ACTION_OPEN_NETWORK };
    ipcam_ui_action_t flip = { .type = IPCAM_UI_ACTION_OPEN_VIDEO_SETTINGS };
    if (!make_home_card(ui, root, 540, 164, 120, 68, 548, 176, 20, 20,
                        574, 168, 78, 24, 548, 198, 104, 28,
                        LV_SYMBOL_CHARGE, "补光灯", "已关闭", &light,
                        &view->light_value)) return -1;
    if (!make_home_card(ui, root, 668, 164, 120, 68, 676, 176, 20, 20,
                        702, 168, 78, 24, 676, 198, 104, 28,
                        LV_SYMBOL_SETTINGS, "视频设置", "640×480 · 15 fps", &video,
                        &view->video_value)) return -1;
    if (!make_home_card(ui, root, 540, 240, 120, 68, 548, 252, 20, 20,
                        574, 244, 78, 24, 548, 274, 104, 28,
                        LV_SYMBOL_IMAGE, "屏幕设置", "亮度 70% · 3 分钟", &screen,
                        &view->screen_value)) return -1;
    if (!make_home_card(ui, root, 668, 240, 120, 68, 676, 252, 20, 20,
                        702, 244, 78, 24, 676, 274, 104, 28,
                        LV_SYMBOL_SD_CARD, "存储状态", "28.4 GB 可用", &storage,
                        &view->storage_value)) return -1;
    if (!make_home_card(ui, root, 540, 316, 120, 68, 548, 328, 20, 20,
                        574, 320, 78, 24, 548, 350, 104, 28,
                        LV_SYMBOL_WIFI, "网络与系统", "有线正常", &network,
                        &view->network_value)) return -1;

    lv_obj_t *flip_card = ipcam_ui_make_action_panel(ui, root, 668, 316, 120, 68,
                                                     &ui->styles.pressed,
                                                     &ui->styles.surface, &flip);
    if (!flip_card) return -1;
    if (!ipcam_ui_make_icon(ui, flip_card, LV_SYMBOL_SHUFFLE, 8, 12, 20, 20,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    if (!ipcam_ui_make_label(ui, flip_card, "画面翻转", 34, 4, 78, 22,
                             ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->flip_horizontal_value = ipcam_ui_make_label(ui, flip_card, "水平关",
                                                       8, 30, 104, 20,
                                                       ipcam_ui_font_small(ui),
                                                       lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                       LV_TEXT_ALIGN_LEFT, 0);
    view->flip_vertical_value = ipcam_ui_make_label(ui, flip_card, "垂直关",
                                                     8, 52, 104, 20,
                                                     ipcam_ui_font_small(ui),
                                                     lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                     LV_TEXT_ALIGN_LEFT, 0);
    if (!view->flip_horizontal_value || !view->flip_vertical_value) return -1;

    if (!ipcam_ui_make_panel(ui, root, 0, 408, IPCAM_UI_SCREEN_WIDTH, 72,
                             &ui->styles.surface)) return -1;
    /* 控制服务返回的失败原因可能比固定成功文案更长；允许两行显示，
     * 避免左下角把中文错误信息裁成“缺字”或只剩半截。 */
    view->feedback = ipcam_ui_make_label(ui, root, "照片已保存", 16, 416, 480, 48,
                                         ipcam_ui_font_small(ui),
                                         lv_color_hex(IPCAM_UI_ACCENT),
                                         LV_TEXT_ALIGN_LEFT, 1);
    if (!view->feedback) return -1;

    ipcam_ui_action_t photo = { .type = IPCAM_UI_ACTION_PHOTO };
    if (!ipcam_ui_make_button(ui, root, 516, 416, 128, 56,
                              &ui->styles.accent_button, &ui->styles.pressed,
                              LV_SYMBOL_IMAGE, "拍照", &photo, NULL, NULL)) return -1;
    ipcam_ui_action_t record = { .type = IPCAM_UI_ACTION_RECORD_TOGGLE };
    view->record_button = ipcam_ui_make_button(ui, root, 656, 416, 128, 56,
                                               &ui->styles.danger_button,
                                               &ui->styles.pressed,
                                               LV_SYMBOL_BULLET, "开始录像", &record,
                                               &view->record_icon,
                                               &view->record_label);
    return view->record_button ? 0 : -1;
}

/* 创建 P02 电脑观看首页；它明确显示访问地址，不再预留视频大窗。 */
static int create_computer_home(ipcam_ui_t *ui, ipcam_ui_home_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (make_home_header(ui, root, view) != 0) return -1;
    if (!ipcam_ui_make_panel(ui, root, 12, 60, 776, 112,
                             &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "电脑观看", 24, 72, 160, 30,
                             ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    ipcam_ui_action_t computer = { .type = IPCAM_UI_ACTION_SHOW_COMPUTER };
    ipcam_ui_action_t local = { .type = IPCAM_UI_ACTION_SHOW_LOCAL };
    view->mode_computer = ipcam_ui_make_button(ui, root, 196, 68, 112, 48,
                                               &ui->styles.accent_button,
                                               &ui->styles.pressed, NULL, "电脑",
                                               &computer, NULL,
                                               &view->mode_computer_label);
    view->mode_local = ipcam_ui_make_button(ui, root, 316, 68, 112, 48,
                                            &ui->styles.pressed,
                                            &ui->styles.surface, NULL, "本地＋电脑",
                                            &local, NULL,
                                            &view->mode_local_label);
    if (!view->mode_computer || !view->mode_local) return -1;
    view->access_helper = ipcam_ui_make_label(ui, root,
                                              "本地预览已关闭，拍照和录像仍可使用。",
                                              24, 124, 392, 28,
                                              ipcam_ui_font_small(ui),
                                              lv_color_hex(IPCAM_UI_WARNING),
                                              LV_TEXT_ALIGN_LEFT, 0);
    view->access_url = ipcam_ui_make_label(ui, root,
                                           "访问地址  http://192.168.1.100:8080/",
                                           432, 124, 336, 28,
                                           ipcam_ui_font_small(ui),
                                           lv_color_hex(IPCAM_UI_ACCENT),
                                           LV_TEXT_ALIGN_LEFT, 0);
    if (!view->access_helper || !view->access_url) return -1;

    ipcam_ui_action_t light = { .type = IPCAM_UI_ACTION_SET_LIGHT_TOGGLE };
    ipcam_ui_action_t video = { .type = IPCAM_UI_ACTION_OPEN_VIDEO_SETTINGS };
    ipcam_ui_action_t flip = { .type = IPCAM_UI_ACTION_OPEN_VIDEO_SETTINGS };
    ipcam_ui_action_t screen = { .type = IPCAM_UI_ACTION_OPEN_SCREEN_SETTINGS };
    ipcam_ui_action_t storage = { .type = IPCAM_UI_ACTION_OPEN_STORAGE };
    ipcam_ui_action_t network = { .type = IPCAM_UI_ACTION_OPEN_NETWORK };
    if (!make_home_card(ui, root, 12, 188, 376, 64, 30, 207, 24, 24,
                        66, 200, 220, 28, 30, 228, 342, 24,
                        LV_SYMBOL_CHARGE, "补光灯", "已关闭", &light,
                        &view->light_value)) return -1;
    if (!make_home_card(ui, root, 412, 188, 376, 64, 430, 207, 24, 24,
                        466, 200, 220, 28, 430, 228, 342, 24,
                        LV_SYMBOL_SETTINGS, "视频设置", "640×480 · 15 fps", &video,
                        &view->video_value)) return -1;
    if (!make_home_card(ui, root, 12, 260, 376, 64, 30, 279, 24, 24,
                        66, 272, 220, 28, 30, 300, 342, 24,
                        LV_SYMBOL_SHUFFLE, "画面翻转", "水平关 · 垂直关", &flip,
                        &view->flip_horizontal_value)) return -1;
    if (!make_home_card(ui, root, 412, 260, 376, 64, 430, 279, 24, 24,
                        466, 272, 220, 28, 430, 300, 342, 24,
                        LV_SYMBOL_IMAGE, "屏幕设置", "亮度 70% · 3 分钟", &screen,
                        &view->screen_value)) return -1;
    if (!make_home_card(ui, root, 12, 332, 376, 64, 30, 351, 24, 24,
                        66, 344, 220, 28, 30, 372, 342, 24,
                        LV_SYMBOL_SD_CARD, "存储状态", "28.4 GB 可用", &storage,
                        &view->storage_value)) return -1;
    if (!make_home_card(ui, root, 412, 332, 376, 64, 430, 351, 24, 24,
                        466, 344, 220, 28, 430, 372, 342, 24,
                        LV_SYMBOL_WIFI, "网络与系统", "有线正常", &network,
                        &view->network_value)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 0, 416, IPCAM_UI_SCREEN_WIDTH, 64,
                             &ui->styles.surface)) return -1;
    /* P02 同样使用两行反馈区，服务层的 SD/录像错误可以完整展示。 */
    view->feedback = ipcam_ui_make_label(ui, root, "电脑端可打开上方地址观看",
                                         16, 424, 480, 40,
                                         ipcam_ui_font_small(ui),
                                         lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                         LV_TEXT_ALIGN_LEFT, 1);
    if (!view->feedback) return -1;
    ipcam_ui_action_t photo = { .type = IPCAM_UI_ACTION_PHOTO };
    if (!ipcam_ui_make_button(ui, root, 516, 420, 128, 52,
                              &ui->styles.accent_button, &ui->styles.pressed,
                              LV_SYMBOL_IMAGE, "拍照", &photo, NULL, NULL)) return -1;
    ipcam_ui_action_t record = { .type = IPCAM_UI_ACTION_RECORD_TOGGLE };
    view->record_button = ipcam_ui_make_button(ui, root, 656, 420, 128, 52,
                                               &ui->styles.danger_button,
                                               &ui->styles.pressed,
                                               LV_SYMBOL_BULLET, "开始录像", &record,
                                               &view->record_icon,
                                               &view->record_label);
    return view->record_button ? 0 : -1;
}

/* 让实际状态一次刷新两个首页；重建页面会破坏触摸对象并造成内存碎片。 */
void ipcam_ui_home_update(ipcam_ui_t *ui, ipcam_ui_home_view_t *view,
                          int computer_mode)
{
    if (!ui || !view) return;
    char text[160];

    if (ui->state.network_link && ui->state.network_ip[0])
        snprintf(text, sizeof(text), "有线已连接 · %s", ui->state.network_ip);
    else
        snprintf(text, sizeof(text), "网线未连接");
    ipcam_ui_set_label(view->network_text, text);
    ipcam_ui_set_label_color(view->network_text,
                             ui->state.network_link ?
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY) :
                             lv_color_hex(IPCAM_UI_WARNING));

    if (ui->state.storage_mounted) {
        char storage[96];
        ipcam_ui_format_bytes(ui->state.storage_available_bytes,
                              storage, sizeof(storage));
        snprintf(text, sizeof(text), "SD 卡 %s 可用", storage);
        ipcam_ui_set_label(view->storage_text, text);
    } else {
        ipcam_ui_set_label(view->storage_text, "SD 卡未插入");
    }

    if (!computer_mode) {
        snprintf(text, sizeof(text), "实测 %.1f fps", (double)ui->state.measured_fps);
        ipcam_ui_set_label(view->fps_text, text);
        snprintf(text, sizeof(text), "%ux%u", (unsigned)ui->state.video_width,
                 (unsigned)ui->state.video_height);
        ipcam_ui_set_label(view->video_resolution, text);
        snprintf(text, sizeof(text), "%.1f×", (double)(ui->state.zoom > 0.0f ?
                                                         ui->state.zoom : 1.0f));
        ipcam_ui_set_label(view->zoom_text, text);
        if (!ui->state.camera_ready)
            ipcam_ui_set_label(view->video_placeholder, "摄像头不可用\n请检查设备");
        else if (!ui->state.video_frame_valid)
            ipcam_ui_set_label(view->video_placeholder, "动态视频占位");
    } else {
        snprintf(text, sizeof(text), "访问地址  http://%s:%u/",
                 ui->state.network_ip[0] ? ui->state.network_ip : "--",
                 (unsigned)ui->state.http_port);
        ipcam_ui_set_label(view->access_url,
                           ui->state.network_link ? text : "网线未连接");
        ipcam_ui_set_label_color(view->access_url,
                                 ui->state.network_link ? lv_color_hex(IPCAM_UI_ACCENT) :
                                 lv_color_hex(IPCAM_UI_WARNING));
        ipcam_ui_set_label(view->access_helper,
                           ui->state.preview_enabled ?
                           "本地预览已开启，电脑端可同时观看。" :
                           "本地预览已关闭，拍照和录像仍可使用。");
    }

    ipcam_ui_set_label(view->light_value,
                       ui->state.light_percent ? "已开启" : "已关闭");
    snprintf(text, sizeof(text), "%ux%u · %u fps",
             (unsigned)ui->state.video_width, (unsigned)ui->state.video_height,
             (unsigned)ui->state.target_fps);
    ipcam_ui_set_label(view->video_value, text);
    snprintf(text, sizeof(text), "亮度 %u%% · %u 分钟",
             (unsigned)ui->state.backlight_percent,
             (unsigned)ui->state.screen_timeout_min);
    ipcam_ui_set_label(view->screen_value, text);
    if (ui->state.storage_mounted) {
        ipcam_ui_format_bytes(ui->state.storage_available_bytes, text, sizeof(text));
        char storage[192];
        snprintf(storage, sizeof(storage), "%s 可用", text);
        ipcam_ui_set_label(view->storage_value, storage);
    } else {
        ipcam_ui_set_label(view->storage_value, "未插入");
    }
    ipcam_ui_set_label(view->network_value,
                       ui->state.network_link ? "有线正常" : "网线未连接");

    snprintf(text, sizeof(text), "水平%s", ui->state.mirror_horizontal ? "开" : "关");
    ipcam_ui_set_label(view->flip_horizontal_value, text);
    snprintf(text, sizeof(text), "垂直%s", ui->state.mirror_vertical ? "开" : "关");
    ipcam_ui_set_label(view->flip_vertical_value, text);

    ipcam_ui_set_selected_button(view->mode_local, view->mode_local_label,
                                 !computer_mode, lv_color_hex(IPCAM_UI_ACCENT),
                                 lv_color_hex(IPCAM_UI_PRESSED));
    ipcam_ui_set_selected_button(view->mode_computer, view->mode_computer_label,
                                 computer_mode, lv_color_hex(IPCAM_UI_ACCENT),
                                 lv_color_hex(IPCAM_UI_PRESSED));

    int recording = ipcam_ui_record_active(ui->state.record_state);
    ipcam_ui_set_label(view->record_label, recording ? "停止录像" : "开始录像");
    ipcam_ui_set_label(view->record_icon, recording ? LV_SYMBOL_STOP : LV_SYMBOL_BULLET);
    if (view->feedback) {
        ipcam_ui_set_label(view->feedback, ui->state.message[0] ? ui->state.message :
                           (computer_mode ? "电脑端可打开上方地址观看" : "照片已保存"));
        lv_color_t message_color = ui->state.message_severity == IPCAM_UI_MESSAGE_ERROR ?
                                    lv_color_hex(IPCAM_UI_WARNING) :
                                    (ui->state.message_severity == IPCAM_UI_MESSAGE_SUCCESS ?
                                     lv_color_hex(IPCAM_UI_ACCENT) :
                                     lv_color_hex(IPCAM_UI_TEXT_SECONDARY));
        ipcam_ui_set_label_color(view->feedback, message_color);
    }
}

/* 页面构造入口由 ipcam_ui.c 调用；computer_mode 对应 .pen 的 P01/P02。 */
lv_obj_t *ipcam_ui_home_create(ipcam_ui_t *ui, ipcam_ui_home_view_t *view,
                               int computer_mode)
{
    if (!ui || !view) return NULL;
    return (computer_mode ? create_computer_home(ui, view) :
            create_local_home(ui, view)) == 0 ? view->root : NULL;
}
