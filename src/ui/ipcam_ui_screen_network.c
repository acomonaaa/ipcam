#include "ipcam_ui_components.h"

#include <stdio.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_WARNING        0xfa7faaU

/* 创建 P07 网络与系统页；系统页作为同一位置的隐藏卡片，切 tab 不重建对象。 */
static int create_network(ipcam_ui_t *ui, ipcam_ui_network_view_t *view)
{
    lv_obj_t *root = ipcam_ui_make_root(ui, 0);
    if (!root) return -1;
    view->root = root;
    if (!ipcam_ui_make_top_bar(ui, root, "网络与系统")) return -1;

    ipcam_ui_action_t network_tab = { .type = IPCAM_UI_ACTION_SELECT_NETWORK_TAB,
                                      .value0 = 0 };
    ipcam_ui_action_t system_tab = { .type = IPCAM_UI_ACTION_SELECT_NETWORK_TAB,
                                     .value0 = 1 };
    view->network_tab = ipcam_ui_make_button(ui, root, 12, 60, 150, 44,
                                             &ui->styles.accent_button,
                                             &ui->styles.pressed, NULL, "网络",
                                             &network_tab, NULL,
                                             &view->network_tab_label);
    view->system_tab = ipcam_ui_make_button(ui, root, 176, 60, 150, 44,
                                            &ui->styles.surface,
                                            &ui->styles.pressed, NULL, "系统",
                                            &system_tab, NULL,
                                            &view->system_tab_label);
    if (!view->network_tab || !view->system_tab) return -1;

    view->network_card = ipcam_ui_make_panel(ui, root, 12, 116, 776, 256,
                                             &ui->styles.surface);
    if (!view->network_card) return -1;
    if (!ipcam_ui_make_icon(ui, view->network_card, LV_SYMBOL_WIFI, 20, 24, 28, 28,
                            lv_color_hex(IPCAM_UI_ACCENT), 0)) return -1;
    if (!ipcam_ui_make_label(ui, view->network_card, "网络状态", 60, 16, 260, 36,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    lv_obj_t *online = ipcam_ui_make_panel(ui, view->network_card, 596, 16, 160, 40,
                                           &ui->styles.pressed);
    if (!online) return -1;
    if (!ipcam_ui_make_panel(ui, online, 16, 12, 16, 16,
                             &ui->styles.toggle_active)) return -1;
    view->online_text = ipcam_ui_make_label(ui, online, "在线", 40, 0, 104, 40,
                                            ipcam_ui_font_small(ui),
                                            lv_color_hex(IPCAM_UI_ACCENT),
                                            LV_TEXT_ALIGN_LEFT, 0);
    if (!view->online_text) return -1;
    if (!ipcam_ui_make_label(ui, view->network_card, "MAC 地址", 20, 80, 160, 26,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->link_value = ipcam_ui_make_label(ui, view->network_card,
                                           "--:--:--:--:--:--", 188, 80, 280, 26,
                                           ipcam_ui_font_small(ui),
                                           lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                           LV_TEXT_ALIGN_LEFT, 0);
    if (!view->link_value) return -1;
    if (!ipcam_ui_make_label(ui, view->network_card, "IP 地址", 20, 124, 160, 26,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->ip_value = ipcam_ui_make_label(ui, view->network_card, "192.168.1.100",
                                         188, 124, 280, 26, ipcam_ui_font_small(ui),
                                         lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                         LV_TEXT_ALIGN_LEFT, 0);
    if (!view->ip_value) return -1;
    if (!ipcam_ui_make_label(ui, view->network_card, "电脑观看", 20, 168, 160, 26,
                             ipcam_ui_font_small(ui), lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->stream_value = ipcam_ui_make_label(ui, view->network_card,
                                             "http://192.168.1.100:8080/stream.mjpg",
                                             188, 168, 540, 26,
                                             ipcam_ui_font_small(ui),
                                             lv_color_hex(IPCAM_UI_ACCENT),
                                             LV_TEXT_ALIGN_LEFT, 0);
    if (!view->stream_value) return -1;
    if (!ipcam_ui_make_label(ui, view->network_card,
                             "地址仅在网络连接时有效，端口由系统配置决定", 20, 212,
                             704, 28, ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;

    view->system_card = ipcam_ui_make_panel(ui, root, 12, 116, 776, 256,
                                            &ui->styles.surface);
    if (!view->system_card) return -1;
    if (!ipcam_ui_make_label(ui, view->system_card, "设备信息", 20, 16, 260, 36,
                             ipcam_ui_font_body(ui), lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    view->system_value = ipcam_ui_make_label(ui, view->system_card,
                                             "型号：ipcam-imx6ull\n版本：0.1.0\nLCD：800×480 · RGB565\nLVGL：9.5.0",
                                             20, 72, 704, 124, ipcam_ui_font_body(ui),
                                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                             LV_TEXT_ALIGN_LEFT, 1);
    if (!view->system_value) return -1;
    lv_obj_add_flag(view->system_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *footer = ipcam_ui_make_panel(ui, root, 12, 388, 776, 32,
                                           &ui->styles.pressed);
    if (!footer) return -1;
    if (!ipcam_ui_make_label(ui, footer, "网络参数只读；修改请通过网页 API 或 camctl", 16, 0,
                             744, 32, ipcam_ui_font_small(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) return -1;
    return 0;
}

/* tab 切换只改变可见性和颜色，避免切换时重新分配网络卡片。 */
void ipcam_ui_network_set_tab(ipcam_ui_network_view_t *view, int system_tab)
{
    if (!view) return;
    view->active_tab = system_tab ? 1 : 0;
    ipcam_ui_set_selected_button(view->network_tab, view->network_tab_label,
                                 !view->active_tab, lv_color_hex(IPCAM_UI_ACCENT),
                                 lv_color_hex(0x3f3849U));
    ipcam_ui_set_selected_button(view->system_tab, view->system_tab_label,
                                 view->active_tab, lv_color_hex(IPCAM_UI_ACCENT),
                                 lv_color_hex(0x3f3849U));
    if (view->network_card) {
        if (view->active_tab) lv_obj_add_flag(view->network_card, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(view->network_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (view->system_card) {
        if (view->active_tab) lv_obj_clear_flag(view->system_card, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(view->system_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 更新网络地址和系统信息；断网时不展示看似可访问的假地址。 */
void ipcam_ui_network_update(ipcam_ui_t *ui, ipcam_ui_network_view_t *view)
{
    if (!ui || !view) return;
    ipcam_ui_network_set_tab(view, view->active_tab);
    ipcam_ui_set_label(view->online_text, ui->state.network_link ? "在线" : "离线");
    ipcam_ui_set_label_color(view->online_text,
                             ui->state.network_link ? lv_color_hex(IPCAM_UI_ACCENT) :
                             lv_color_hex(IPCAM_UI_WARNING));
    ipcam_ui_set_label(view->link_value, ui->state.network_mac[0] ?
                       ui->state.network_mac : "--:--:--:--:--:--");
    ipcam_ui_set_label(view->ip_value, ui->state.network_link && ui->state.network_ip[0] ?
                       ui->state.network_ip : "--");
    char stream[112];
    if (ui->state.network_link && ui->state.network_ip[0])
        snprintf(stream, sizeof(stream), "http://%s:%u/stream.mjpg",
                 ui->state.network_ip, (unsigned)ui->state.http_port);
    else
        snprintf(stream, sizeof(stream), "网络未连接");
    ipcam_ui_set_label(view->stream_value, stream);
    char system[256];
    snprintf(system, sizeof(system), "型号：%s\n版本：%s\nLCD：800×480 · RGB565\nLVGL：9.5.0",
             ui->state.model[0] ? ui->state.model : "--",
             ui->state.swver[0] ? ui->state.swver : "--");
    ipcam_ui_set_label(view->system_value, system);
}

lv_obj_t *ipcam_ui_network_create(ipcam_ui_t *ui, ipcam_ui_network_view_t *view)
{
    if (!ui || !view) return NULL;
    return create_network(ui, view) == 0 ? view->root : NULL;
}
