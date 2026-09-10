/* 组件分配失败必须能在板端日志中指出控件类别和坐标，便于区分页面逻辑与内存问题。 */
#define IPCAM_LOG_MODULE "UI  "
#include "ipcam_log.h"
#include "ipcam_ui_components.h"

#include <string.h>

#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_SURFACE        0x150f23U
#define IPCAM_UI_PRESSED        0x3f3849U
#define IPCAM_UI_INK            0x101418U

/* 给新屏幕创建固定画布根对象；坐标严格对应最新 .pen 的 800×480 页面。 */
lv_obj_t *ipcam_ui_make_root(ipcam_ui_t *ui, int black_background)
{
    if (!ui) return NULL;
    lv_obj_t *root = lv_obj_create(NULL);
    if (!root) {
        MLOGE("create UI root failed: black=%d\n", black_background);
        return NULL;
    }
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
    if (!panel) {
        MLOGE("create UI panel failed: pos=%d,%d size=%dx%d\n",
              x, y, width, height);
        return NULL;
    }
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
    if (!label) {
        MLOGE("create UI label failed: pos=%d,%d size=%dx%d text=%s\n",
              x, y, width, height, text ? text : "<null>");
        return NULL;
    }
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
    if (!button) {
        MLOGE("create UI button failed: pos=%d,%d size=%dx%d\n",
              x, y, width, height);
        return NULL;
    }
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(button, 0, 0);
    if (normal) lv_obj_add_style(button, normal, 0);
    if (pressed) lv_obj_add_style(button, pressed, LV_STATE_PRESSED);
    if (action && ipcam_ui_bind_action(ui, button, LV_EVENT_CLICKED, action) != 0) {
        MLOGE("bind UI button action failed: pos=%d,%d\n", x, y);
        lv_obj_del(button);
        return NULL;
    }

    lv_obj_t *icon_obj = NULL;
    lv_obj_t *label_obj = NULL;
    /* 深色 surface 上用白色内容，荧光按钮上用深色内容，保持文字对比度。 */
    int dark_content = normal == &ui->styles.accent_button ||
                       normal == &ui->styles.danger_button;
    lv_color_t content_color = lv_color_hex(dark_content ? IPCAM_UI_INK :
                                            IPCAM_UI_TEXT_PRIMARY);
    if (icon && *icon) {
        icon_obj = ipcam_ui_make_icon(ui, button, icon, 12, 0, 32, height,
                                      content_color, 0);
        label_obj = ipcam_ui_make_label(ui, button, text, 48, 0,
                                       width - 56, height,
                                       ipcam_ui_font_body(ui),
                                       content_color,
                                       LV_TEXT_ALIGN_LEFT, 0);
    } else {
        label_obj = ipcam_ui_make_label(ui, button, text, 0, 0, width, height,
                                       ipcam_ui_font_body(ui),
                                       content_color,
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
    /* lv_obj_create 的默认 flag 可能随 LVGL 配置变化；显式设置才能保证
     * 卡片、单选行和二态开关在不同主题/版本下都能成为触控命中目标。 */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    if (pressed) lv_obj_add_style(panel, pressed, LV_STATE_PRESSED);
    if (action && ipcam_ui_bind_action(ui, panel, LV_EVENT_CLICKED, action) != 0) {
        lv_obj_del(panel);
        return NULL;
    }
    return panel;
}

/*
 * 统一刷新标签；LVGL 的 set_text 会重新分配文本并使对象失效，因此值未变
 * 时必须跳过调用。业务层只管生成目标字符串，变更判断集中在这里。
 */
void ipcam_ui_set_label(lv_obj_t *label, const char *text)
{
    if (!label) return;
    const char *next = text ? text : "";
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, next) != 0) lv_label_set_text(label, next);
}

/* 更新动态提示的语义颜色；状态颜色与 .pen 的 accent/success/warning 对齐。 */
void ipcam_ui_set_label_color(lv_obj_t *label, lv_color_t color)
{
    if (label && !lv_color_eq(lv_obj_get_style_text_color(label, 0), color))
        lv_obj_set_style_text_color(label, color, 0);
}

/* 更新分段按钮的 selected 颜色，同时修正文字颜色以保持 RGB565 对比度。 */
void ipcam_ui_set_selected_button(lv_obj_t *button, lv_obj_t *label,
                                  int selected, lv_color_t selected_color,
                                  lv_color_t normal_color)
{
    if (!button) return;
    lv_color_t background = selected ? selected_color : normal_color;
    if (!lv_color_eq(lv_obj_get_style_bg_color(button, 0), background))
        lv_obj_set_style_bg_color(button, background, 0);
    if (label) {
        lv_color_t text_color = selected ? lv_color_hex(IPCAM_UI_INK) :
                                         lv_color_hex(IPCAM_UI_TEXT_SECONDARY);
        if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), text_color))
            lv_obj_set_style_text_color(label, text_color, 0);
    }
}

/* 创建设置页顶部栏；返回按钮统一使用 48×48 触控命中区。 */
lv_obj_t *ipcam_ui_make_top_bar(ipcam_ui_t *ui, lv_obj_t *root,
                                const char *title)
{
    if (!ui || !root) return NULL;
    lv_obj_t *bar = ipcam_ui_make_panel(ui, root, 0, 0,
                                       IPCAM_UI_SCREEN_WIDTH, 48,
                                       &ui->styles.surface);
    if (!bar) return NULL;
    ipcam_ui_action_t back = { .type = IPCAM_UI_ACTION_BACK };
    lv_obj_t *icon = NULL;
    if (!ipcam_ui_make_button(ui, bar, 4, 0, 48, 48,
                              &ui->styles.surface, &ui->styles.pressed,
                              LV_SYMBOL_LEFT, NULL, &back, &icon, NULL))
        return NULL;
    /* 顶部标题也是页面创建链的一部分；显式检查可避免标题分配失败后
     * 仍把半成品页面交给导航层，最终表现为点击设置页没有任何响应。 */
    if (!ipcam_ui_make_label(ui, bar, title, 56, 0, 700, 48,
                             ipcam_ui_font_title(ui),
                             lv_color_hex(IPCAM_UI_TEXT_PRIMARY),
                             LV_TEXT_ALIGN_LEFT, 0))
        return NULL;
    return bar;
}

/* 创建单选行；行本身是触控目标，圆点只承担状态表达以避免点击区域过小。 */
int ipcam_ui_make_radio_row(ipcam_ui_t *ui, lv_obj_t *parent,
                            int x, int y, int width, int height,
                            const char *text, const ipcam_ui_action_t *action,
                            lv_obj_t **row_out, lv_obj_t **dot_out,
                            lv_obj_t **label_out)
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
    lv_obj_t *label = ipcam_ui_make_label(ui, row, text, 48, 0, width - 64, height,
                                          ipcam_ui_font_body(ui),
                                          lv_color_hex(IPCAM_UI_TEXT_SECONDARY),
                                          LV_TEXT_ALIGN_LEFT, 0);
    if (!label) {
        lv_obj_del(row);
        return -1;
    }
    *row_out = row;
    *dot_out = dot;
    if (label_out) *label_out = label;
    return 0;
}

/* 单选行的背景、圆点和文字必须同步切换，避免只改变颜色却留下旧状态。 */
void ipcam_ui_set_radio_selected(lv_obj_t *row, lv_obj_t *dot,
                                 lv_obj_t *label, int selected)
{
    lv_color_t row_color = lv_color_hex(selected ? IPCAM_UI_PRESSED :
                                         IPCAM_UI_SURFACE);
    if (row) {
        if (!lv_color_eq(lv_obj_get_style_bg_color(row, 0), row_color))
            lv_obj_set_style_bg_color(row, row_color, 0);
    }
    lv_color_t dot_color = lv_color_hex(selected ? IPCAM_UI_ACCENT :
                                         IPCAM_UI_SURFACE);
    if (dot) {
        if (!lv_color_eq(lv_obj_get_style_bg_color(dot, 0), dot_color))
            lv_obj_set_style_bg_color(dot, dot_color, 0);
    }
    if (label) {
        lv_color_t text_color = lv_color_hex(selected ? IPCAM_UI_TEXT_PRIMARY :
                                              IPCAM_UI_TEXT_SECONDARY);
        if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), text_color))
            lv_obj_set_style_text_color(label, text_color, 0);
    }
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
    int target_x = enabled ? max_x : 8;
    if (lv_obj_get_x(knob) != target_x) lv_obj_set_x(knob, target_x);
    /* LVGL 没有公开 style 身份查询，背景颜色比较足以避免常见重复失效。 */
    lv_color_t target_color = lv_obj_get_style_bg_color(toggle, 0);
    lv_color_t expected = lv_color_hex(enabled ? IPCAM_UI_ACCENT : IPCAM_UI_SURFACE);
    if (!lv_color_eq(target_color, expected)) {
        lv_obj_remove_style(toggle, &styles->toggle_track, 0);
        lv_obj_remove_style(toggle, &styles->toggle_active, 0);
        lv_obj_add_style(toggle, enabled ? &styles->toggle_active :
                         &styles->toggle_track, 0);
    }
    (void)height;
}
