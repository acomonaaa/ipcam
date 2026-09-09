#ifndef IPCAM_UI_H
#define IPCAM_UI_H

/*
 * IPCam LCD 的 LVGL 9 视觉层公开接口。
 *
 * 页面坐标、文本和颜色来自 design/IPCam_LCD_UI_sentry.pen；本模块只负责
 * 创建控件、刷新结构化状态和发出轻量动作，不直接访问 framebuffer、evdev、
 * 摄像头、存储或网络。上层应在唯一的 LVGL 线程中调用本接口。
 */

#include <lvgl.h>

#include <stddef.h>
#include <stdint.h>

#define IPCAM_UI_SCREEN_WIDTH  1024
#define IPCAM_UI_SCREEN_HEIGHT 600

typedef enum ipcam_ui_screen_e {
    IPCAM_UI_SCREEN_LOCAL_HOME = 0,
    IPCAM_UI_SCREEN_COMPUTER_HOME,
    IPCAM_UI_SCREEN_FULLSCREEN,
    IPCAM_UI_SCREEN_VIDEO_SETTINGS,
    IPCAM_UI_SCREEN_SCREEN_SETTINGS,
    IPCAM_UI_SCREEN_STORAGE,
    IPCAM_UI_SCREEN_NETWORK,
    IPCAM_UI_SCREEN_STATE
} ipcam_ui_screen_t;

typedef enum ipcam_ui_record_state_e {
    IPCAM_UI_RECORD_IDLE = 0,
    IPCAM_UI_RECORD_STARTING,
    IPCAM_UI_RECORDING,
    IPCAM_UI_RECORD_STOPPING,
    IPCAM_UI_RECORD_ERROR
} ipcam_ui_record_state_t;

typedef enum ipcam_ui_message_severity_e {
    IPCAM_UI_MESSAGE_INFO = 0,
    IPCAM_UI_MESSAGE_SUCCESS,
    IPCAM_UI_MESSAGE_WARNING,
    IPCAM_UI_MESSAGE_ERROR
} ipcam_ui_message_severity_t;

/*
 * 动作只携带已验证的值，不携带任何硬件句柄。
 * value0～value5 的含义由 action 决定；例如 APPLY_VIDEO 依次为宽、高、
 * 帧率、JPEG 质量、水平镜像、垂直镜像。回调应把动作放入业务队列，不能
 * 在 LVGL 事件线程中执行文件保存、录像控制或硬件 I/O。
 */
typedef enum ipcam_ui_action_type_e {
    IPCAM_UI_ACTION_SHOW_LOCAL = 1,
    IPCAM_UI_ACTION_SHOW_COMPUTER,
    IPCAM_UI_ACTION_SHOW_FULLSCREEN,
    IPCAM_UI_ACTION_OPEN_VIDEO_SETTINGS,
    IPCAM_UI_ACTION_OPEN_SCREEN_SETTINGS,
    IPCAM_UI_ACTION_OPEN_STORAGE,
    IPCAM_UI_ACTION_OPEN_NETWORK,
    IPCAM_UI_ACTION_OPEN_STATE,
    IPCAM_UI_ACTION_BACK,
    IPCAM_UI_ACTION_SET_PREVIEW,
    IPCAM_UI_ACTION_SET_LIGHT,
    IPCAM_UI_ACTION_SET_LIGHT_TOGGLE,
    IPCAM_UI_ACTION_PHOTO,
    IPCAM_UI_ACTION_RECORD_TOGGLE,
    IPCAM_UI_ACTION_RECORD_START,
    IPCAM_UI_ACTION_RECORD_STOP,
    IPCAM_UI_ACTION_RESET_VIEW,
    IPCAM_UI_ACTION_SELECT_VIDEO_PRESET,
    IPCAM_UI_ACTION_SELECT_JPEG_QUALITY,
    IPCAM_UI_ACTION_TOGGLE_MIRROR_HORIZONTAL,
    IPCAM_UI_ACTION_TOGGLE_MIRROR_VERTICAL,
    IPCAM_UI_ACTION_APPLY_VIDEO,
    IPCAM_UI_ACTION_SET_BACKLIGHT,
    IPCAM_UI_ACTION_SET_SCREEN_TIMEOUT,
    IPCAM_UI_ACTION_SELECT_NETWORK_TAB
} ipcam_ui_action_type_t;

typedef struct ipcam_ui_action_s {
    ipcam_ui_action_type_t type;
    int32_t value0;
    int32_t value1;
    int32_t value2;
    int32_t value3;
    int32_t value4;
    int32_t value5;
} ipcam_ui_action_t;

typedef int (*ipcam_ui_action_cb)(const ipcam_ui_action_t *action, void *opaque);

typedef struct ipcam_ui_actions_s {
    ipcam_ui_action_cb on_action;
    void *opaque;
} ipcam_ui_actions_t;

/*
 * 字体由板级工程注入，便于使用裁剪后的 Noto Sans SC；为空时回退到
 * LV_FONT_DEFAULT。使用同一套回退字体可以先在没有中文字库的 simulator 中
 * 验证布局，再在目标机替换字体资源而不改页面代码。
 */
typedef struct ipcam_ui_fonts_s {
    const lv_font_t *body;
    const lv_font_t *small;
    const lv_font_t *title;
    const lv_font_t *icon;
} ipcam_ui_fonts_t;

typedef struct ipcam_ui_state_s {
    uint8_t camera_ready;
    uint8_t display_ready;
    uint8_t touch_ready;
    uint8_t video_frame_valid;

    uint8_t network_link;
    char network_ip[64];
    uint16_t http_port;

    uint8_t storage_mounted;
    uint64_t storage_total_bytes;
    uint64_t storage_available_bytes;

    uint16_t video_width;
    uint16_t video_height;
    uint8_t target_fps;
    uint8_t jpeg_quality;
    float measured_fps;
    uint8_t mirror_horizontal;
    uint8_t mirror_vertical;
    uint8_t preview_enabled;
    uint8_t preview_view_enabled;
    float zoom;

    uint8_t backlight_percent;
    uint8_t screen_timeout_min;
    uint8_t light_percent;

    ipcam_ui_record_state_t record_state;
    uint32_t record_segment_no;
    uint64_t record_elapsed_ms;
    uint64_t record_frame_count;

    char model[32];
    char swver[16];
    char message[128];
    ipcam_ui_message_severity_t message_severity;
} ipcam_ui_state_t;

typedef struct ipcam_ui_s ipcam_ui_t;

/* 填充与 .pen 默认示例一致的状态；调用方可再覆盖真实板级数据。 */
void ipcam_ui_state_init(ipcam_ui_state_t *state);

/* 创建八个独立屏幕；调用前必须完成 lv_init() 和显示设备初始化。 */
ipcam_ui_t *ipcam_ui_create(const ipcam_ui_actions_t *actions,
                            const ipcam_ui_fonts_t *fonts);
void ipcam_ui_destroy(ipcam_ui_t *ui);

/* 切换页面；所有页面对象在 create 时一次性创建，避免反复分配造成碎片。 */
int ipcam_ui_load_screen(ipcam_ui_t *ui, ipcam_ui_screen_t screen);
ipcam_ui_screen_t ipcam_ui_get_screen(const ipcam_ui_t *ui);

/* 只在 LVGL 所属线程调用；状态复制后刷新所有已创建页面，便于后续切页。 */
int ipcam_ui_update(ipcam_ui_t *ui, const ipcam_ui_state_t *state);

#endif /* IPCAM_UI_H */
