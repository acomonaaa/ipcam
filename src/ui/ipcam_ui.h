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

/* 设计提交 b3aa1df 的所有页面统一以 800×480 为根画布。 */
#define IPCAM_UI_SCREEN_WIDTH  800
#define IPCAM_UI_SCREEN_HEIGHT 480

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
    IPCAM_UI_ACTION_SELECT_NETWORK_TAB,
    IPCAM_UI_ACTION_SCREEN_KEEP_AWAKE,
    IPCAM_UI_ACTION_SCREEN_SLEEP,
    /* 翻转独立提交，避免视频其它参数不合法时连带阻断翻转生效。 */
    IPCAM_UI_ACTION_APPLY_MIRROR,
    IPCAM_UI_ACTION_STORAGE_FORMAT_REQUEST,
    IPCAM_UI_ACTION_STORAGE_FORMAT_CONFIRM,
    IPCAM_UI_ACTION_STORAGE_FORMAT_CANCEL
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
 * 字体由服务层注入，正文、辅助文字和标题通常使用裁剪后的 CJK 字库；图标
 * 使用包含 LV_SYMBOL_* 私有区字形的字体。某一项为空时回退到 LV_FONT_DEFAULT，
 * 这样缺少外部字库时仍能创建页面，且不会把字体文件路径、加载线程带入 UI 层。
 */
typedef struct ipcam_ui_fonts_s {
    const lv_font_t *body;
    const lv_font_t *small;
    const lv_font_t *title;
    const lv_font_t *icon;
} ipcam_ui_fonts_t;

/* 当前页面的视频目标尺寸；非直播页面返回 active=0，display 可停止转换。 */
typedef struct ipcam_ui_video_viewport_s {
    uint8_t active;
    uint16_t width;
    uint16_t height;
} ipcam_ui_video_viewport_t;

typedef struct ipcam_ui_state_s {
    uint8_t camera_ready;
    uint8_t display_ready;
    uint8_t touch_ready;
    uint8_t video_frame_valid;

    uint8_t network_link;
    char network_mac[32];
    char network_ip[64];
    uint16_t http_port;

    uint8_t storage_mounted;
    uint64_t storage_total_bytes;
    uint64_t storage_used_bytes;
    uint64_t storage_available_bytes;
    uint8_t storage_format_supported;
    uint8_t storage_formatting;
    char storage_mount_path[256];
    char storage_device[256];
    char storage_fs_type[32];

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
    uint8_t screen_sleep_prompt;
    uint8_t screen_sleep_remaining_sec;
    uint8_t screen_sleeping;
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

/* 创建 UI 管理器；调用前必须完成 lv_init() 和显示设备初始化，首次只创建 P01。 */
ipcam_ui_t *ipcam_ui_create(const ipcam_ui_actions_t *actions,
                            const ipcam_ui_fonts_t *fonts);
void ipcam_ui_destroy(ipcam_ui_t *ui);

/*
 * 切换页面；调用方必须位于 LVGL 线程且不在对象事件回调中。
 * 页面切换会释放旧页面，只保留当前页面，避免嵌入式堆因反复切页累积对象。
 */
int ipcam_ui_load_screen(ipcam_ui_t *ui, ipcam_ui_screen_t screen);
ipcam_ui_screen_t ipcam_ui_get_screen(const ipcam_ui_t *ui);
/*
 * 在 LVGL 主循环安全边界执行事件回调登记的页面切换。
 * 返回 1 表示处理了一次请求，0 表示没有待处理请求，-1 表示创建失败。
 */
int ipcam_ui_process_navigation(ipcam_ui_t *ui);
/* 读取 P01/P02/P03 的固定视频窗口尺寸，只能在 LVGL 所属线程调用。 */
int ipcam_ui_get_video_viewport(const ipcam_ui_t *ui,
                                ipcam_ui_video_viewport_t *viewport);

/* 只在 LVGL 所属线程调用；状态复制后刷新所有已创建页面，便于后续切页。 */
int ipcam_ui_update(ipcam_ui_t *ui, const ipcam_ui_state_t *state);

/* 将 LVGL port 的 RGB565 帧绑定到 P01/P03 的视频窗口；不复制帧数据。 */
void ipcam_ui_set_video_source(ipcam_ui_t *ui, const lv_image_dsc_t *source,
                               int valid);

#endif /* IPCAM_UI_H */
