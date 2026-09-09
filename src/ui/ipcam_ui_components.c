#include "ipcam_ui_components.h"

#include <string.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_INK            0x101418U

/* 给新屏幕创建固定画布根对象；固定坐标来自 approved .pen 的 1024×600 页面。 */
lv_obj_t *ipcam_ui_make_root(ipcam_ui_t *ui, int black_background)
{
    if (!ui) return NULL;
    lv_obj_t *root = lv_obj_create(NULL);
    if (!root) return NULL;
    lv_obj_set_size(root, IPCAM_UI_SCREEN_WIDTH, IPCAM_UI_SCREEN_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(root, black_background ? &ui->styles.black_screen :
                     &ui->styles.screen, 0);
    return root;
}

/* 创建非滚动容器；页面使用绝对坐标以保持与 .pen handoff 的像素对应关系。 */
lv_obj_t *ipcam_ui_make_panel(ipcam_ui_t *ui, lv_obj_t *parent,
                              int x, int y, int width, int height,
                              const lv_style_t *style)
{
    if (!ui || !parent || width <= 0 || height <= 0) return NULL;
    lv_obj_t *panel = lv_obj_create(parent);
    if (!panel) return NULL;
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    if (style) lv_obj_add_style(panel, style, 0);
    return panel;
}

/* 创建固定区域文本；wrap 只对设计中明确的多行错误/说明文案开启。 */
lv_obj_t *ipcam_ui_make_label(ipcam_ui_t *ui, lv_obj_t *parent,
                              const char *text, int x, int y,
                              int width, int height, const lv_font_t *font,
                              lv_color_t color, lv_text_align_t align,
                              int wrap)
{
    if (!ui || !parent || width <= 0 || height <= 0) return NULL;
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return NULL;
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_text(label, text ? text : "");
    lv_label_set_long_mode(label, wrap ? LV_LABEL_LONG_MODE_WRAP :
                           LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font ? font : LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

/* 用 LVGL 内置 symbol 字体承载简单线性图标，避免在 i.MX6ULL 上引入位图缩放。 */
lv_obj_t *ipcam_ui_make_icon(ipcam_ui_t *ui, lv_obj_t *parent,
                             const char *symbol, int x, int y,
                             int width, int height, lv_color_t color,
                             int font_size_hint)
{
    (void)font_size_hint;
    return ipcam_ui_make_label(ui, parent, symbol, x, y, width, height,
                               ipcam_ui_font_icon(ui), color,
                               LV_TEXT_ALIGN_CENTER, 0);
}

/* 创建带 pressed 样式的按钮；业务动作通过绑定表转交上层，不在回调中阻塞。 */
lv_obj_t *ipcam_ui_make_button(ipcam_ui_t *ui, lv_obj_t *parent,
                               int x, int y, int width, int height,
                               const lv_style_t *normal,
                               const lv_style_t *pressed,
                               const char *icon, const char *text,
                               const ipcam_ui_action_t *action,
                               lv_obj_t **icon_out, lv_obj_t **label_out)
{
    if (!ui || !parent || width <= 0 || height <= 0) return NULL;
    lv_obj_t *button = lv_button_create(parent);
    if (!button) return NULL;
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(button, 0, 0);
    if (normal) lv_obj_add_style(button, normal, 0);
    if (pressed) lv_obj_add_style(button, pressed, LV_STATE_PRESSED);
    if (action && ipcam_ui_bind_action(ui, button, LV_EVENT_CLICKED, action) != 0) {
        lv_obj_del(button);
        return NULL;
    }

    lv_obj_t *icon_obj = NULL;
    lv_obj_t *label_obj = NULL;
    if (icon && *icon) {
        icon_obj = ipcam_ui_make_icon(ui, button, icon, 12, 0, 32, height,
                                      lv_color_hex(IPCAM_UI_INK), 0);
        label_obj = ipcam_ui_make_label(ui, button, text, 48, 0,
                                       width - 56, height,
                                       ipcam_ui_font_body(ui),
                                       lv_color_hex(IPCAM_UI_INK),
                                       LV_TEXT_ALIGN_LEFT, 0);
    } else {
        label_obj = ipcam_ui_make_label(ui, button, text, 0, 0, width, height,
                                       ipcam_ui_font_body(ui),
                                       lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                                       LV_TEXT_ALIGN_CENTER, 0);
    }
    if (!label_obj || (icon && *icon && !icon_obj)) {
        lv_obj_del(button);
        return NULL;
    }
    if (icon_out) *icon_out = icon_obj;
    if (label_out) *label_out = label_obj;
    return button;
}

/* 创建可点击卡片；卡片内部的图标、标题和值由页面继续按 .pen 坐标填充。 */
lv_obj_t *ipcam_ui_make_action_panel(ipcam_ui_t *ui, lv_obj_t *parent,
                                     int x, int y, int width, int height,
                                     const lv_style_t *normal,
                                     const lv_style_t *pressed,
                                     const ipcam_ui_action_t *action)
{
    if (!ui || !parent) return NULL;
    lv_obj_t *panel = ipcam_ui_make_panel(ui, parent, x, y, width, height, normal);
    if (!panel) return NULL;
    if (pressed) lv_obj_add_style(panel, pressed, LV_STATE_PRESSED);
    if (action && ipcam_ui_bind_action(ui, panel, LV_EVENT_CLICKED, action) != 0) {
        lv_obj_del(panel);
        return NULL;
    }
    return panel;
}

/* 统一刷新标签，避免业务状态更新时重复判断空对象。 */
void ipcam_ui_set_label(lv_obj_t *label, const char *text)
{
    if (label) lv_label_set_text(label, text ? text : "");
}

/* 更新动态提示的语义颜色；状态颜色与 .pen 的 accent/success/warning 对齐。 */
void ipcam_ui_set_label_color(lv_obj_t *label, lv_color_t color)
{
    if (label) lv_obj_set_style_text_color(label, color, 0);
}

/* 更新分段按钮的 selected 颜色，同时修正文字颜色以保持 RGB565 对比度。 */
void ipcam_ui_set_selected_button(lv_obj_t *button, lv_obj_t *label,
                                  int selected, lv_color_t selected_color,
                                  lv_color_t normal_color)
{
    if (!button) return;
    lv_obj_set_style_bg_color(button,
                              selected ? selected_color : normal_color, 0);
    if (label) {
        lv_obj_set_style_text_color(label,
                                    selected ? lv_color_hex(IPCAM_UI_INK) :
                                    lv_color_hex(IPCAM_UI_TEXT_SECONDARY), 0);
    }
}

/* 创建设置页顶部栏；返回按钮统一使用 48×48 触控命中区。 */
lv_obj_t *ipcam_ui_make_top_bar(ipcam_ui_t *ui, lv_obj_t *root,
                                const char *title)
{
    if (!ui || !root) return NULL;
    lv_obj_t *bar = ipcam_ui_make_panel(ui, root, 0, 0,
                                       IPCAM_UI_SCREEN_WIDTH, 56,
                                       &ui->styles.surface);
    if (!bar) return NULL;
    ipcam_ui_action_t back = { .type = IPCAM_UI_ACTION_BACK };
    lv_obj_t *icon = NULL;
    if (!ipcam_ui_make_button(ui, bar, 8, 4, 48, 48,
                              &ui->styles.surface, &ui->styles.pressed,
                              LV_SYMBOL_LEFT, NULL, &back, &icon, NULL))
        return NULL;
    ipcam_ui_make_label(ui, bar, title, 64, 0, 700, 56,
                        ipcam_ui_font_title(ui),
                        lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                        LV_TEXT_ALIGN_LEFT, 0);
    return bar;
}

/* 创建单选行；行本身是触控目标，圆点只承担状态表达以避免点击区域过小。 */
int ipcam_ui_make_radio_row(ipcam_ui_t *ui, lv_obj_t *parent,
                            int x, int y, int width, int height,
                            const char *text, const ipcam_ui_action_t *action,
                            lv_obj_t **row_out, lv_obj_t **dot_out)
{
    if (!ui || !parent || !row_out || !dot_out) return -1;
    lv_obj_t *row = ipcam_ui_make_action_panel(ui, parent, x, y, width, height,
                                               &ui->styles.row,
                                               &ui->styles.pressed, action);
    if (!row) return -1;
    lv_obj_t *dot = ipcam_ui_make_panel(ui, row, 16, (height - 16) / 2,
                                        16, 16, &ui->styles.surface);
    if (!dot) {
        lv_obj_del(row);
        return -1;
    }
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    if (!ipcam_ui_make_label(ui, row, text, 48, 0, width - 64, height,
                             ipcam_ui_font_body(ui),
                             lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                             LV_TEXT_ALIGN_LEFT, 0)) {
        lv_obj_del(row);
        return -1;
    }
    *row_out = row;
    *dot_out = dot;
    return 0;
}

/* 创建低开销二态开关；使用普通对象而不是复杂主题，确保软件渲染可控。 */
lv_obj_t *ipcam_ui_make_toggle(ipcam_ui_t *ui, lv_obj_t *parent,
                               int x, int y, int width, int height,
                               const ipcam_ui_action_t *action,
                               lv_obj_t **knob_out)
{
    if (!ui || !parent || !knob_out || width < 48 || height < 32) return NULL;
    lv_obj_t *toggle = ipcam_ui_make_action_panel(ui, parent, x, y, width, height,
                                                  &ui->styles.toggle_track,
                                                  &ui->styles.pressed, action);
    if (!toggle) return NULL;
    int knob_size = height - 16;
    if (knob_size < 24) knob_size = 24;
    lv_obj_t *knob = ipcam_ui_make_panel(ui, toggle, 8, 8,
                                        knob_size, knob_size,
                                        &ui->styles.toggle_knob);
    if (!knob) {
        lv_obj_del(toggle);
        return NULL;
    }
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    *knob_out = knob;
    return toggle;
}

/* 根据真实状态移动开关圆点；不触发动作，避免状态回放形成控制回环。 */
void ipcam_ui_toggle_set(lv_obj_t *toggle, lv_obj_t *knob, int enabled,
                         const ipcam_ui_styles_t *styles)
{
    if (!toggle || !knob || !styles) return;
    int width = lv_obj_get_width(toggle);
    int height = lv_obj_get_height(toggle);
    int knob_size = lv_obj_get_width(knob);
    int max_x = width - knob_size - 8;
    if (max_x < 8) max_x = 8;
    lv_obj_set_x(knob, enabled ? max_x : 8);
    lv_obj_remove_style(toggle, &styles->toggle_track, 0);
    lv_obj_remove_style(toggle, &styles->toggle_active, 0);
    lv_obj_add_style(toggle, enabled ? &styles->toggle_active :
                     &styles->toggle_track, 0);
    (void)height;
}
