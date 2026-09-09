#include "ipcam_ui_styles.h"

#include <string.h>

/* IPCam LCD 设计令牌；颜色必须与 .pen 中的 variables 保持一一对应。 */
#define IPCAM_UI_BG             0x1f1633U
#define IPCAM_UI_SURFACE        0x150f23U
#define IPCAM_UI_PRESSED        0x3f3849U
#define IPCAM_UI_BORDER         0x362d59U
#define IPCAM_UI_TEXT_PRIMARY   0xffffffU
#define IPCAM_UI_TEXT_SECONDARY 0xbdb8c0U
#define IPCAM_UI_ACCENT         0xc2ef4eU
#define IPCAM_UI_WARNING        0xfa7faaU
#define IPCAM_UI_DANGER         0xfa7faaU
#define IPCAM_UI_INK            0x101418U

/* 将可选字体补齐为统一回退字体，避免某个字号未注入时出现空指针。 */
static const lv_font_t *font_or_default(const lv_font_t *font)
{
    return font ? font : LV_FONT_DEFAULT;
}

/* 初始化所有共享样式；组件只引用这些对象，不在每个页面重复写颜色规则。 */
void ipcam_ui_styles_init(ipcam_ui_styles_t *styles,
                          const ipcam_ui_fonts_t *fonts)
{
    if (!styles) return;

    memset(styles, 0, sizeof(*styles));
    const lv_font_t *body = font_or_default(fonts ? fonts->body : NULL);
    const lv_font_t *small = font_or_default(fonts ? fonts->small : NULL);
    const lv_font_t *title = font_or_default(fonts ? fonts->title : NULL);
    const lv_font_t *icon = font_or_default(fonts ? fonts->icon : NULL);

    lv_style_init(&styles->screen);
    lv_style_set_bg_color(&styles->screen, lv_color_hex(IPCAM_UI_BG));
    lv_style_set_bg_opa(&styles->screen, LV_OPA_COVER);
    lv_style_set_border_width(&styles->screen, 0);
    lv_style_set_radius(&styles->screen, 0);
    lv_style_set_pad_all(&styles->screen, 0);

    lv_style_init(&styles->black_screen);
    lv_style_set_bg_color(&styles->black_screen, lv_color_black());
    lv_style_set_bg_opa(&styles->black_screen, LV_OPA_COVER);
    lv_style_set_border_width(&styles->black_screen, 0);
    lv_style_set_radius(&styles->black_screen, 0);
    lv_style_set_pad_all(&styles->black_screen, 0);

    lv_style_init(&styles->surface);
    lv_style_set_bg_color(&styles->surface, lv_color_hex(IPCAM_UI_SURFACE));
    lv_style_set_bg_opa(&styles->surface, LV_OPA_COVER);
    lv_style_set_border_width(&styles->surface, 0);
    lv_style_set_radius(&styles->surface, 8);
    lv_style_set_pad_all(&styles->surface, 0);

    lv_style_init(&styles->pressed);
    lv_style_set_bg_color(&styles->pressed, lv_color_hex(IPCAM_UI_PRESSED));
    lv_style_set_bg_opa(&styles->pressed, LV_OPA_COVER);
    lv_style_set_border_width(&styles->pressed, 0);
    lv_style_set_radius(&styles->pressed, 8);
    lv_style_set_pad_all(&styles->pressed, 0);

    lv_style_init(&styles->accent_button);
    lv_style_set_bg_color(&styles->accent_button, lv_color_hex(IPCAM_UI_ACCENT));
    lv_style_set_bg_opa(&styles->accent_button, LV_OPA_COVER);
    lv_style_set_text_color(&styles->accent_button, lv_color_hex(IPCAM_UI_INK));
    lv_style_set_text_font(&styles->accent_button, body);
    lv_style_set_border_width(&styles->accent_button, 0);
    lv_style_set_radius(&styles->accent_button, 8);
    lv_style_set_pad_all(&styles->accent_button, 0);

    lv_style_init(&styles->danger_button);
    lv_style_set_bg_color(&styles->danger_button, lv_color_hex(IPCAM_UI_DANGER));
    lv_style_set_bg_opa(&styles->danger_button, LV_OPA_COVER);
    lv_style_set_text_color(&styles->danger_button, lv_color_hex(IPCAM_UI_INK));
    lv_style_set_text_font(&styles->danger_button, body);
    lv_style_set_border_width(&styles->danger_button, 0);
    lv_style_set_radius(&styles->danger_button, 8);
    lv_style_set_pad_all(&styles->danger_button, 0);

    lv_style_init(&styles->label_primary);
    lv_style_set_text_color(&styles->label_primary, lv_color_hex(IPCAM_UI_TEXT_PRIMARY));
    lv_style_set_text_font(&styles->label_primary, body);

    lv_style_init(&styles->label_secondary);
    lv_style_set_text_color(&styles->label_secondary, lv_color_hex(IPCAM_UI_TEXT_SECONDARY));
    lv_style_set_text_font(&styles->label_secondary, body);

    lv_style_init(&styles->label_accent);
    lv_style_set_text_color(&styles->label_accent, lv_color_hex(IPCAM_UI_ACCENT));
    lv_style_set_text_font(&styles->label_accent, body);

    lv_style_init(&styles->label_success);
    lv_style_set_text_color(&styles->label_success, lv_color_hex(IPCAM_UI_ACCENT));
    lv_style_set_text_font(&styles->label_success, body);

    lv_style_init(&styles->label_warning);
    lv_style_set_text_color(&styles->label_warning, lv_color_hex(IPCAM_UI_WARNING));
    lv_style_set_text_font(&styles->label_warning, body);

    lv_style_init(&styles->row);
    lv_style_set_bg_color(&styles->row, lv_color_hex(IPCAM_UI_SURFACE));
    lv_style_set_bg_opa(&styles->row, LV_OPA_COVER);
    lv_style_set_text_color(&styles->row, lv_color_hex(IPCAM_UI_TEXT_SECONDARY));
    lv_style_set_text_font(&styles->row, body);
    lv_style_set_border_width(&styles->row, 0);
    lv_style_set_radius(&styles->row, 6);
    lv_style_set_pad_all(&styles->row, 0);

    lv_style_init(&styles->row_selected);
    lv_style_set_bg_color(&styles->row_selected, lv_color_hex(IPCAM_UI_PRESSED));
    lv_style_set_bg_opa(&styles->row_selected, LV_OPA_COVER);
    lv_style_set_text_color(&styles->row_selected, lv_color_hex(IPCAM_UI_TEXT_PRIMARY));
    lv_style_set_text_font(&styles->row_selected, body);
    lv_style_set_border_width(&styles->row_selected, 0);
    lv_style_set_radius(&styles->row_selected, 6);
    lv_style_set_pad_all(&styles->row_selected, 0);

    lv_style_init(&styles->slider_track);
    lv_style_set_bg_color(&styles->slider_track, lv_color_hex(IPCAM_UI_BORDER));
    lv_style_set_bg_opa(&styles->slider_track, LV_OPA_COVER);
    lv_style_set_border_width(&styles->slider_track, 0);
    lv_style_set_radius(&styles->slider_track, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&styles->slider_track, 0);

    lv_style_init(&styles->slider_indicator);
    lv_style_set_bg_color(&styles->slider_indicator, lv_color_hex(IPCAM_UI_ACCENT));
    lv_style_set_bg_opa(&styles->slider_indicator, LV_OPA_COVER);
    lv_style_set_border_width(&styles->slider_indicator, 0);
    lv_style_set_radius(&styles->slider_indicator, LV_RADIUS_CIRCLE);

    lv_style_init(&styles->slider_knob);
    lv_style_set_bg_color(&styles->slider_knob, lv_color_hex(IPCAM_UI_TEXT_PRIMARY));
    lv_style_set_bg_opa(&styles->slider_knob, LV_OPA_COVER);
    lv_style_set_border_width(&styles->slider_knob, 0);
    lv_style_set_radius(&styles->slider_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&styles->slider_knob, 0);

    lv_style_init(&styles->toggle_track);
    lv_style_set_bg_color(&styles->toggle_track, lv_color_hex(IPCAM_UI_SURFACE));
    lv_style_set_bg_opa(&styles->toggle_track, LV_OPA_COVER);
    lv_style_set_border_width(&styles->toggle_track, 0);
    lv_style_set_radius(&styles->toggle_track, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&styles->toggle_track, 0);

    lv_style_init(&styles->toggle_active);
    lv_style_set_bg_color(&styles->toggle_active, lv_color_hex(IPCAM_UI_ACCENT));
    lv_style_set_bg_opa(&styles->toggle_active, LV_OPA_COVER);
    lv_style_set_border_width(&styles->toggle_active, 0);
    lv_style_set_radius(&styles->toggle_active, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&styles->toggle_active, 0);

    lv_style_init(&styles->toggle_knob);
    lv_style_set_bg_color(&styles->toggle_knob, lv_color_hex(IPCAM_UI_INK));
    lv_style_set_bg_opa(&styles->toggle_knob, LV_OPA_COVER);
    lv_style_set_border_width(&styles->toggle_knob, 0);
    lv_style_set_radius(&styles->toggle_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&styles->toggle_knob, 0);

    /* small/title 由组件按语义直接使用；保留变量读取以明确字体注入契约。 */
    (void)small;
    (void)title;
    (void)icon;
}
