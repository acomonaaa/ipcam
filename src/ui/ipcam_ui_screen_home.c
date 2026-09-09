#include "ipcam_ui_components.h"

#include <stdio.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_WARNING        0xfa7faaU
#define IPCAM_UI_INK            0x101418U

/* 创建首页顶部状态栏；网络断开时地址由 update 函数主动隐藏，避免展示假地址。 */
static int make_home_header(ipcam_ui_t *ui, lv_obj_t *root,
                            ipcam_ui_home_view_t *view)
{
    lv_obj_t *bar = ipcam_ui_make_panel(ui, root, 0, 0,
                                        IPCAM_UI_SCREEN_WIDTH, 56,
                                        &ui->styles.surface);
    if (!bar) return -1;
    if (!ipcam_ui_make_label(ui, bar, "IPCam", 24, 0, 180, 56,
                             ipcam_ui_font_title(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    lv_obj_t *network = ipcam_ui_make_panel(ui, bar, 232, 12, 286, 32,
                                            &ui->styles.pressed);
    if (!network) return -1;
    if (!ipcam_ui_make_icon(ui, network, LV_SYMBOL_WIFI, 12, 8, 16, 16,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    view->network_text = ipcam_ui_make_label(ui, network, "有线已连接 · 192.168.1.100",
                                             36, 0, 238, 32,
                                             ipcam_ui_font_small(ui),
                                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                             LV_TEXT_ALIGN_LEFT, 0);
    if (!view->network_text) return -1;

    if (!ipcam_ui_make_icon(ui, bar, LV_SYMBOL_SD_CARD, 796, 20, 18, 18,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    view->storage_text = ipcam_ui_make_label(ui, bar, "SD 卡 28.4 GB 可用",
                                             820, 12, 180, 32,
                                             ipcam_ui_font_small(ui),
                                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                             LV_TEXT_ALIGN_LEFT, 0);
    return view->storage_text ? 0 : -1;
}

/* 创建设置卡片的公共骨架；同一组件同时覆盖 P01 的窄卡和 P02 的宽卡。 */
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

/* 创建首页的小型图标按钮；图标单独设置白色，避免普通按钮文字样式覆盖。 */
static lv_obj_t *make_home_icon_button(ipcam_ui_t *ui, lv_obj_t *root,
                                       int x, int y, int width, int height,
                                       const char *icon,
                                       const ipcam_ui_action_t *action)
{
    lv_obj_t *button = ipcam_ui_make_action_panel(ui, root, x, y, width, height,
                                                  &ui->styles.surface,
                                                  &ui->styles.pressed, action);
    if (!button) return NULL;
    return ipcam_ui_make_icon(ui, button, icon, 0, 0, width, height,
                              lv_color_hex(IPCAM_UI_TEXT_PRIMARY), 0) ? button : NULL;
}

/* 创建底部拍照/录像动作区，两个按钮均保留 56px 高触控目标。 */
static int make_home_bottom(ipcam_ui_t *ui, lv_obj_t *root,
                            ipcam_ui_home_view_t *view, int computer_mode)
{
    if (!ipcam_ui_make_panel(ui, root, 0, 520, IPCAM_UI_SCREEN_WIDTH, 80,
                             &ui->styles.surface)) return -1;
    view->feedback = ipcam_ui_make_label(ui, root,
                                         computer_mode ? "电脑端可打开上方地址观看" : "照片已保存",
                                         24, 536, 620, 28,
                                         ipcam_ui_font_small(ui),
                                         lv_color_hex(computer_mode ? IPCAM_UI_TEXT_SECONDARY :
                                                      IPCAM_UI_ACCENT),
                                         LV_TEXT_ALIGN_LEFT, 0);
    if (!view->feedback) return -1;

    ipcam_ui_action_t photo = { .type = IPCAM_UI_ACTION_PHOTO };
    if (!ipcam_ui_make_button(ui, root, 700, 528, 144, 56,
                              &ui->styles.accent_button, &ui->styles.pressed,
                              LV_SYMBOL_IMAGE, "拍照", &photo, NULL, NULL)) return -1;
    ipcam_ui_action_t record = { .type = IPCAM_UI_ACTION_RECORD_TOGGLE };
    view->record_button = ipcam_ui_make_button(ui, root, 856, 528, 144, 56,
                                               &ui->styles.danger_button,
                                               &ui->styles.pressed,
                                               LV_SYMBOL_BULLET, "开始录像", &record,
                                               &view->record_icon,
                                               &view->record_label);
    return view->record_button ? 0 : -1;
}

/* 创建 P01 本地预览首页：左侧视频小窗、右侧设置卡片、底部媒体动作。 */
static int create_local_home(ipcam_ui_t *ui, ipcam_ui_home_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (make_home_header(ui, root, view) != 0) return -1;

    if (!ipcam_ui_make_panel(ui, root, 16, 72, 640, 432, &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "本地预览", 32, 84, 220, 30,
                             ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->fps_text = ipcam_ui_make_label(ui, root, "实测 14.8 fps", 436, 84, 184, 30,
                                         ipcam_ui_font_small(ui),
                                         lv_color_hex(IPCAM_UI_ACCENT),
                                         LV_TEXT_ALIGN_RIGHT, 0);
    if (!view->fps_text) return -1;
    if (!ipcam_ui_make_panel(ui, root, 32, 120, 608, 342, &ui->styles.black_screen)) return -1;
    view->video_placeholder = ipcam_ui_make_label(ui, root, "动态视频占位\n等待画面",
                                                  196, 238, 280, 78,
                                                  ipcam_ui_font_body(ui),
                                                  lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                  LV_TEXT_ALIGN_CENTER, 1);
    if (!view->video_placeholder) return -1;
    if (!ipcam_ui_make_label(ui, root, "640×480", 48, 474, 160, 28,
                             ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->zoom_text = ipcam_ui_make_label(ui, root, "1.0×", 528, 474, 80, 28,
                                          ipcam_ui_font_small(ui),
                                          lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                          LV_TEXT_ALIGN_RIGHT, 0);
    if (!view->zoom_text) return -1;
    ipcam_ui_action_t fullscreen = { .type = IPCAM_UI_ACTION_SHOW_FULLSCREEN };
    ipcam_ui_action_t reset = { .type = IPCAM_UI_ACTION_RESET_VIEW };
    if (!make_home_icon_button(ui, root, 564, 132, 32, 32,
                               LV_SYMBOL_EYE_OPEN, &fullscreen)) return -1;
    if (!make_home_icon_button(ui, root, 604, 132, 32, 32,
                               LV_SYMBOL_REFRESH, &reset)) return -1;

    if (!ipcam_ui_make_panel(ui, root, 672, 72, 336, 432, &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "观看模式", 688, 88, 100, 28,
                             ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    ipcam_ui_action_t local_mode = { .type = IPCAM_UI_ACTION_SHOW_LOCAL };
    ipcam_ui_action_t computer_mode = { .type = IPCAM_UI_ACTION_SHOW_COMPUTER };
    view->mode_local = ipcam_ui_make_button(ui, root, 688, 120, 152, 32,
                                            &ui->styles.accent_button,
                                            &ui->styles.pressed, NULL, "本地＋电脑",
                                            &local_mode, NULL,
                                            &view->mode_local_label);
    view->mode_computer = ipcam_ui_make_button(ui, root, 840, 120, 152, 32,
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
    if (!make_home_card(ui, root, 688, 176, 144, 104, 700, 190, 22, 22,
                        732, 188, 88, 26, 700, 226, 120, 42,
                        LV_SYMBOL_CHARGE, "补光灯", "已关闭", &light,
                        &view->light_value)) return -1;
    if (!make_home_card(ui, root, 856, 176, 144, 104, 868, 190, 22, 22,
                        900, 188, 88, 26, 868, 226, 120, 42,
                        LV_SYMBOL_SETTINGS, "视频设置", "640×480 · 15 fps", &video,
                        &view->video_value)) return -1;
    if (!make_home_card(ui, root, 688, 296, 144, 104, 700, 310, 22, 22,
                        732, 308, 88, 26, 700, 346, 120, 42,
                        LV_SYMBOL_IMAGE, "屏幕设置", "亮度 70% · 3 分钟", &screen,
                        &view->screen_value)) return -1;
    if (!make_home_card(ui, root, 856, 296, 144, 104, 868, 310, 22, 22,
                        900, 308, 88, 26, 868, 346, 120, 42,
                        LV_SYMBOL_SD_CARD, "存储", "28.4 GB 可用", &storage,
                        &view->storage_value)) return -1;
    if (!make_home_card(ui, root, 688, 416, 144, 104, 700, 430, 22, 22,
                        732, 428, 88, 26, 700, 466, 120, 42,
                        LV_SYMBOL_WIFI, "网络与系统", "有线正常", &network,
                        &view->network_value)) return -1;
    lv_obj_t *flip_card = ipcam_ui_make_action_panel(ui, root, 856, 416, 144, 104,
                                                     &ui->styles.surface,
                                                     &ui->styles.pressed, &flip);
    if (!flip_card) return -1;
    if (!ipcam_ui_make_icon(ui, flip_card, LV_SYMBOL_SHUFFLE, 12, 14, 20, 20,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    if (!ipcam_ui_make_label(ui, flip_card, "画面翻转", 40, 12, 92, 22,
                             ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->flip_horizontal_value = ipcam_ui_make_label(ui, flip_card, "水平镜像 关",
                                                       12, 46, 120, 22,
                                                       ipcam_ui_font_small(ui),
                                                       lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                       LV_TEXT_ALIGN_LEFT, 0);
    view->flip_vertical_value = ipcam_ui_make_label(ui, flip_card, "垂直翻转 关",
                                                     12, 70, 120, 22,
                                                     ipcam_ui_font_small(ui),
                                                     lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                                     LV_TEXT_ALIGN_LEFT, 0);
    if (!view->flip_horizontal_value || !view->flip_vertical_value) return -1;
    return make_home_bottom(ui, root, view, 0);
}

/* 创建 P02 电脑观看首页；页面不创建实时视频对象，明确表达 LCD 预览已关闭。 */
static int create_computer_home(ipcam_ui_t *ui, ipcam_ui_home_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (make_home_header(ui, root, view) != 0) return -1;
    if (!ipcam_ui_make_panel(ui, root, 16, 72, 992, 136, &ui->styles.surface)) return -1;
    if (!ipcam_ui_make_label(ui, root, "电脑观看", 32, 88, 180, 30,
                             ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    ipcam_ui_action_t computer = { .type = IPCAM_UI_ACTION_SHOW_COMPUTER };
    ipcam_ui_action_t local = { .type = IPCAM_UI_ACTION_SHOW_LOCAL };
    view->mode_computer = ipcam_ui_make_button(ui, root, 232, 88, 152, 32,
                                               &ui->styles.accent_button,
                                               &ui->styles.pressed, NULL, "电脑",
                                               &computer, NULL,
                                               &view->mode_computer_label);
    view->mode_local = ipcam_ui_make_button(ui, root, 392, 88, 152, 32,
                                            &ui->styles.pressed,
                                            &ui->styles.surface, NULL, "本地＋电脑",
                                            &local, NULL,
                                            &view->mode_local_label);
    if (!view->mode_computer || !view->mode_local) return -1;
    view->access_helper = ipcam_ui_make_label(ui, root,
                                              "本地预览已关闭，拍照和录像仍可使用。",
                                              32, 136, 510, 28,
                                              ipcam_ui_font_small(ui),
                                              lv_color_hex(IPCAM_UI_WARNING),
                                              LV_TEXT_ALIGN_LEFT, 0);
    view->access_url = ipcam_ui_make_label(ui, root,
                                           "访问地址  http://192.168.1.100:8080/",
                                           584, 136, 390, 28,
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
    if (!make_home_card(ui, root, 16, 232, 312, 128, 34, 250, 26, 26,
                        74, 248, 230, 30, 34, 296, 276, 32,
                        LV_SYMBOL_CHARGE, "补光灯", "已关闭", &light,
                        &view->light_value)) return -1;
    if (!make_home_card(ui, root, 356, 232, 312, 128, 374, 250, 26, 26,
                        414, 248, 230, 30, 374, 296, 276, 32,
                        LV_SYMBOL_SETTINGS, "视频设置", "640×480 · 15 fps", &video,
                        &view->video_value)) return -1;
    if (!make_home_card(ui, root, 696, 232, 312, 128, 714, 250, 26, 26,
                        754, 248, 230, 30, 714, 296, 276, 32,
                        LV_SYMBOL_SHUFFLE, "画面翻转", "水平关 · 垂直关", &flip,
                        &view->network_value)) return -1;
    if (!make_home_card(ui, root, 16, 376, 312, 128, 34, 394, 26, 26,
                        74, 392, 230, 30, 34, 440, 276, 32,
                        LV_SYMBOL_IMAGE, "屏幕设置", "亮度 70% · 3 分钟", &screen,
                        &view->screen_value)) return -1;
    if (!make_home_card(ui, root, 356, 376, 312, 128, 374, 394, 26, 26,
                        414, 392, 230, 30, 374, 440, 276, 32,
                        LV_SYMBOL_SD_CARD, "存储状态", "28.4 GB 可用", &storage,
                        &view->storage_value)) return -1;
    if (!make_home_card(ui, root, 696, 376, 312, 128, 714, 394, 26, 26,
                        754, 392, 230, 30, 714, 440, 276, 32,
                        LV_SYMBOL_WIFI, "网络与系统", "有线正常", &network,
                        &view->network_value)) return -1;
    return make_home_bottom(ui, root, view, 1);
}

/* 根据状态刷新两个首页；只改文本/颜色，不重建控件，避免切页和定时更新产生碎片。 */
void ipcam_ui_home_update(ipcam_ui_t *ui, ipcam_ui_home_view_t *view,
                          int computer_mode)
{
    if (!ui || !view) return;
    char text[160];
    if (ui->state.network_link && ui->state.network_ip[0]) {
        snprintf(text, sizeof(text), "有线已连接 · %s", ui->state.network_ip);
    } else {
        snprintf(text, sizeof(text), "网线未连接");
    }
    ipcam_ui_set_label(view->network_text, text);
    ipcam_ui_set_label_color(view->network_text,
                             ui->state.network_link ?
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY) :
                             lv_color_hex(IPCAM_UI_WARNING));

    if (ui->state.storage_mounted) {
        ipcam_ui_format_bytes(ui->state.storage_available_bytes, text, sizeof(text));
        char storage[96];
        snprintf(storage, sizeof(storage), "SD 卡 %s 可用", text);
        ipcam_ui_set_label(view->storage_text, storage);
    } else {
        ipcam_ui_set_label(view->storage_text, "SD 卡未插入");
    }
    if (!computer_mode) {
        snprintf(text, sizeof(text), "实测 %.1f fps", (double)ui->state.measured_fps);
        ipcam_ui_set_label(view->fps_text, text);
        snprintf(text, sizeof(text), "%.1f×", (double)(ui->state.zoom > 0.0f ? ui->state.zoom : 1.0f));
        ipcam_ui_set_label(view->zoom_text, text);
        if (!ui->state.camera_ready) {
            ipcam_ui_set_label(view->video_placeholder, "摄像头不可用\n请检查设备");
        } else if (ui->state.video_frame_valid) {
            ipcam_ui_set_label(view->video_placeholder, "动态视频");
        } else {
            ipcam_ui_set_label(view->video_placeholder, "动态视频占位\n等待画面");
        }
    } else {
        snprintf(text, sizeof(text), "访问地址  http://%s:%u/",
                 ui->state.network_ip[0] ? ui->state.network_ip : "--",
                 (unsigned)ui->state.http_port);
        ipcam_ui_set_label(view->access_url, ui->state.network_link ? text : "网线未连接");
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
    ipcam_ui_format_bytes(ui->state.storage_available_bytes, text, sizeof(text));
    ipcam_ui_set_label(view->storage_value,
                       ui->state.storage_mounted ? text : "未插入");
    ipcam_ui_set_label(view->network_value,
                       ui->state.network_link ? "有线正常" : "网线未连接");
    snprintf(text, sizeof(text), "水平镜像 %s",
             ui->state.mirror_horizontal ? "开" : "关");
    ipcam_ui_set_label(view->flip_horizontal_value, text);
    snprintf(text, sizeof(text), "垂直翻转 %s",
             ui->state.mirror_vertical ? "开" : "关");
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
