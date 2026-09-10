#ifndef IPCAM_UI_COMPONENTS_H
#define IPCAM_UI_COMPONENTS_H

#include "ipcam_ui_internal.h"

lv_obj_t *ipcam_ui_make_root(ipcam_ui_t *ui, int black_background);
lv_obj_t *ipcam_ui_make_panel(ipcam_ui_t *ui, lv_obj_t *parent,
                              int x, int y, int width, int height,
                              const lv_style_t *style);
lv_obj_t *ipcam_ui_make_label(ipcam_ui_t *ui, lv_obj_t *parent,
                              const char *text, int x, int y,
                              int width, int height, const lv_font_t *font,
                              lv_color_t color, lv_text_align_t align,
                              int wrap);
lv_obj_t *ipcam_ui_make_icon(ipcam_ui_t *ui, lv_obj_t *parent,
                             const char *symbol, int x, int y,
                             int width, int height, lv_color_t color,
                             int font_size_hint);
lv_obj_t *ipcam_ui_make_button(ipcam_ui_t *ui, lv_obj_t *parent,
                               int x, int y, int width, int height,
                               const lv_style_t *normal,
                               const lv_style_t *pressed,
                               const char *icon, const char *text,
                               const ipcam_ui_action_t *action,
                               lv_obj_t **icon_out, lv_obj_t **label_out);
lv_obj_t *ipcam_ui_make_action_panel(ipcam_ui_t *ui, lv_obj_t *parent,
                                     int x, int y, int width, int height,
                                     const lv_style_t *normal,
                                     const lv_style_t *pressed,
                                     const ipcam_ui_action_t *action);
void ipcam_ui_set_label(lv_obj_t *label, const char *text);
void ipcam_ui_set_label_color(lv_obj_t *label, lv_color_t color);
void ipcam_ui_set_selected_button(lv_obj_t *button, lv_obj_t *label,
                                  int selected, lv_color_t selected_color,
                                  lv_color_t normal_color);
lv_obj_t *ipcam_ui_make_top_bar(ipcam_ui_t *ui, lv_obj_t *root,
                                const char *title);
int ipcam_ui_make_radio_row(ipcam_ui_t *ui, lv_obj_t *parent,
                            int x, int y, int width, int height,
                            const char *text, const ipcam_ui_action_t *action,
                            lv_obj_t **row_out, lv_obj_t **dot_out,
                            lv_obj_t **label_out);
lv_obj_t *ipcam_ui_make_toggle(ipcam_ui_t *ui, lv_obj_t *parent,
                               int x, int y, int width, int height,
                               const ipcam_ui_action_t *action,
                               lv_obj_t **knob_out);
void ipcam_ui_toggle_set(lv_obj_t *toggle, lv_obj_t *knob, int enabled,
                         const ipcam_ui_styles_t *styles);

#endif /* IPCAM_UI_COMPONENTS_H */
