#ifndef IPCAM_UI_INTERNAL_H
#define IPCAM_UI_INTERNAL_H

#include "ipcam_ui.h"
#include "ipcam_ui_styles.h"

#include <stdbool.h>

#define IPCAM_UI_MAX_ACTION_BINDINGS 96

typedef struct ipcam_ui_action_binding_s {
    ipcam_ui_t *ui;
    ipcam_ui_action_t action;
    lv_event_code_t event_code;
} ipcam_ui_action_binding_t;

typedef struct ipcam_ui_home_view_s {
    lv_obj_t *root;
    lv_obj_t *video_surface;
    lv_obj_t *video_image;
    lv_obj_t *network_text;
    lv_obj_t *storage_text;
    lv_obj_t *fps_text;
    lv_obj_t *video_resolution;
    lv_obj_t *zoom_text;
    lv_obj_t *video_placeholder;
    lv_obj_t *access_helper;
    lv_obj_t *access_url;
    lv_obj_t *mode_local;
    lv_obj_t *mode_local_label;
    lv_obj_t *mode_computer;
    lv_obj_t *mode_computer_label;
    lv_obj_t *light_value;
    lv_obj_t *video_value;
    lv_obj_t *screen_value;
    lv_obj_t *storage_value;
    lv_obj_t *network_value;
    lv_obj_t *flip_horizontal_value;
    lv_obj_t *flip_vertical_value;
    lv_obj_t *feedback;
    lv_obj_t *record_button;
    lv_obj_t *record_icon;
    lv_obj_t *record_label;
} ipcam_ui_home_view_t;

typedef struct ipcam_ui_fullscreen_view_s {
    lv_obj_t *root;
    lv_obj_t *video_surface;
    lv_obj_t *video_image;
    lv_obj_t *video_placeholder;
    lv_obj_t *zoom_value;
    lv_obj_t *recording_pill;
    lv_obj_t *recording_text;
} ipcam_ui_fullscreen_view_t;

typedef struct ipcam_ui_video_view_s {
    lv_obj_t *root;
    lv_obj_t *lock_notice;
    lv_obj_t *resolution_rows[3];
    lv_obj_t *resolution_dots[3];
    lv_obj_t *resolution_labels[3];
    lv_obj_t *fps_rows[3];
    lv_obj_t *fps_dots[3];
    lv_obj_t *fps_labels[3];
    lv_obj_t *jpeg_rows[2];
    lv_obj_t *jpeg_dots[2];
    lv_obj_t *jpeg_labels[2];
    lv_obj_t *mirror_horizontal_toggle;
    lv_obj_t *mirror_horizontal_knob;
    lv_obj_t *mirror_vertical_toggle;
    lv_obj_t *mirror_vertical_knob;
} ipcam_ui_video_view_t;

typedef struct ipcam_ui_screen_view_s {
    lv_obj_t *root;
    lv_obj_t *brightness_slider;
    lv_obj_t *brightness_value;
    lv_obj_t *timeout_rows[5];
    lv_obj_t *timeout_dots[5];
    lv_obj_t *timeout_labels[5];
} ipcam_ui_screen_view_t;

typedef struct ipcam_ui_storage_view_s {
    lv_obj_t *root;
    lv_obj_t *card_value;
    lv_obj_t *card_capacity;
    lv_obj_t *used_value;
    lv_obj_t *free_value;
    lv_obj_t *usage_fill;
    lv_obj_t *usage_percent;
    lv_obj_t *device_value;
    lv_obj_t *filesystem_value;
    lv_obj_t *mount_value;
    lv_obj_t *record_state;
    lv_obj_t *record_segment;
    lv_obj_t *format_button;
    lv_obj_t *format_overlay;
    lv_obj_t *format_card;
    lv_obj_t *format_status;
} ipcam_ui_storage_view_t;

typedef struct ipcam_ui_network_view_s {
    lv_obj_t *root;
    lv_obj_t *network_tab;
    lv_obj_t *network_tab_label;
    lv_obj_t *system_tab;
    lv_obj_t *system_tab_label;
    lv_obj_t *network_card;
    lv_obj_t *system_card;
    lv_obj_t *online_text;
    lv_obj_t *link_value;
    lv_obj_t *ip_value;
    lv_obj_t *stream_value;
    lv_obj_t *system_value;
    int active_tab;
} ipcam_ui_network_view_t;

typedef struct ipcam_ui_state_view_s {
    lv_obj_t *root;
    lv_obj_t *startup_title;
    lv_obj_t *startup_text;
    lv_obj_t *camera_title;
    lv_obj_t *camera_text;
    lv_obj_t *frame_title;
    lv_obj_t *frame_text;
    lv_obj_t *touch_title;
    lv_obj_t *touch_text;
    lv_obj_t *persistent_text;
} ipcam_ui_state_view_t;

typedef struct ipcam_ui_video_pending_s {
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint8_t jpeg_quality;
    uint8_t mirror_horizontal;
    uint8_t mirror_vertical;
} ipcam_ui_video_pending_t;

struct ipcam_ui_s {
    ipcam_ui_actions_t actions;
    ipcam_ui_fonts_t fonts;
    ipcam_ui_styles_t styles;
    ipcam_ui_state_t state;
    ipcam_ui_screen_t screen;
    ipcam_ui_screen_t home_screen;

    ipcam_ui_home_view_t local_home;
    ipcam_ui_home_view_t computer_home;
    ipcam_ui_fullscreen_view_t fullscreen;
    ipcam_ui_video_view_t video;
    ipcam_ui_screen_view_t screen_settings;
    ipcam_ui_storage_view_t storage;
    ipcam_ui_network_view_t network;
    ipcam_ui_state_view_t state_view;

    /* 全局顶层休眠提示覆盖当前页面，避免每个 P01～P08 重复创建一套弹窗。 */
    lv_obj_t *sleep_overlay;
    lv_obj_t *sleep_card;
    lv_obj_t *sleep_countdown;
    lv_obj_t *sleep_keep_button;
    lv_obj_t *sleep_sleep_button;

    ipcam_ui_video_pending_t video_pending;
    int video_pending_valid;
    int video_dirty;

    /*
     * 触摸事件回调中只登记目标页面，不立即删除/创建 LVGL 根对象。
     * LVGL 事件派发尚未返回时销毁当前页面会使回调链和 user_data 失效，
     * 因而导航必须延后到主循环的安全边界执行。
     */
    ipcam_ui_screen_t pending_screen;
    int navigation_pending;

    ipcam_ui_action_binding_t bindings[IPCAM_UI_MAX_ACTION_BINDINGS];
    size_t binding_count;
};

/* 组件工厂在绑定事件时使用，返回 0 表示绑定成功。 */
int ipcam_ui_bind_action(ipcam_ui_t *ui, lv_obj_t *obj,
                         lv_event_code_t event_code,
                         const ipcam_ui_action_t *action);
int ipcam_ui_dispatch_action(ipcam_ui_t *ui,
                             const ipcam_ui_action_t *action);

const lv_font_t *ipcam_ui_font_body(const ipcam_ui_t *ui);
const lv_font_t *ipcam_ui_font_small(const ipcam_ui_t *ui);
const lv_font_t *ipcam_ui_font_title(const ipcam_ui_t *ui);
const lv_font_t *ipcam_ui_font_icon(const ipcam_ui_t *ui);

void ipcam_ui_format_bytes(uint64_t bytes, char *buf, size_t buf_sz);
void ipcam_ui_format_duration(uint64_t elapsed_ms, char *buf, size_t buf_sz);
const char *ipcam_ui_record_state_text(ipcam_ui_record_state_t state);
int ipcam_ui_record_active(ipcam_ui_record_state_t state);
void ipcam_ui_set_radio_selected(lv_obj_t *row, lv_obj_t *dot,
                                 lv_obj_t *label, int selected);

lv_obj_t *ipcam_ui_home_create(ipcam_ui_t *ui, ipcam_ui_home_view_t *view,
                               int computer_mode);
void ipcam_ui_home_update(ipcam_ui_t *ui, ipcam_ui_home_view_t *view,
                          int computer_mode);

lv_obj_t *ipcam_ui_fullscreen_create(ipcam_ui_t *ui,
                                     ipcam_ui_fullscreen_view_t *view);
void ipcam_ui_fullscreen_update(ipcam_ui_t *ui,
                                ipcam_ui_fullscreen_view_t *view);

lv_obj_t *ipcam_ui_video_create(ipcam_ui_t *ui, ipcam_ui_video_view_t *view);
void ipcam_ui_video_update(ipcam_ui_t *ui, ipcam_ui_video_view_t *view);

lv_obj_t *ipcam_ui_screen_settings_create(ipcam_ui_t *ui,
                                          ipcam_ui_screen_view_t *view);
void ipcam_ui_screen_settings_update(ipcam_ui_t *ui,
                                     ipcam_ui_screen_view_t *view);

lv_obj_t *ipcam_ui_storage_create(ipcam_ui_t *ui,
                                  ipcam_ui_storage_view_t *view);
void ipcam_ui_storage_update(ipcam_ui_t *ui,
                             ipcam_ui_storage_view_t *view);
void ipcam_ui_storage_format_prompt(ipcam_ui_t *ui, int visible);

lv_obj_t *ipcam_ui_network_create(ipcam_ui_t *ui,
                                  ipcam_ui_network_view_t *view);
void ipcam_ui_network_update(ipcam_ui_t *ui,
                             ipcam_ui_network_view_t *view);
void ipcam_ui_network_set_tab(ipcam_ui_network_view_t *view, int system_tab);

lv_obj_t *ipcam_ui_state_create(ipcam_ui_t *ui, ipcam_ui_state_view_t *view);
void ipcam_ui_state_update(ipcam_ui_t *ui, ipcam_ui_state_view_t *view);

/* 创建并刷新全局休眠提示；调用均发生在 LVGL 线程。 */
int ipcam_ui_sleep_prompt_create(ipcam_ui_t *ui);
void ipcam_ui_sleep_prompt_update(ipcam_ui_t *ui);

#endif /* IPCAM_UI_INTERNAL_H */
