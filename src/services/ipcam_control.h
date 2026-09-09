#ifndef IPCAM_CONTROL_H
#define IPCAM_CONTROL_H

/*
 * 固件侧统一控制契约。
 *
 * GUI、HTTP 和 camctl 都只能通过这组命令/状态结构访问媒体服务，
 * 避免某个入口绕过录像锁定或把“已保存”误报成“已生效”。实现位于
 * services 层，底层服务仍通过各自公开接口连接，不直接触碰内部线程状态。
 */

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "ipcam_display.h"
#include "ipcam_capture.h"
#include "ipcam_light.h"
#include "ipcam_record.h"
#include "ipcam_ringbuffer.h"
#include "ipcam_screen.h"

typedef struct ipcam_control_video_s {
    uint16_t width;
    uint16_t height;
    uint8_t target_fps;
    uint8_t jpeg_quality;
} ipcam_control_video_t;

typedef struct ipcam_control_capabilities_s {
    /* 首版只公布实测基线；未实测档位不能从接口“猜测开放”。 */
    ipcam_control_video_t video[4];
    size_t video_count;
    uint8_t jpeg_quality_min;
    uint8_t jpeg_quality_max;
    uint8_t mirror_horizontal;
    uint8_t mirror_vertical;
    uint8_t preview_zoom;
    float preview_zoom_min;
    float preview_zoom_max;
    uint8_t touch_points;
    uint8_t backlight;
    uint8_t light;
    uint8_t screen_timeout;
    uint32_t record_segment_seconds;
} ipcam_control_capabilities_t;

typedef struct ipcam_control_status_s {
    ipcam_control_video_t video;
    uint8_t mirror_horizontal;
    uint8_t mirror_vertical;
    uint8_t preview_enabled;
    int preview_view_enabled;
    float preview_zoom;
    float preview_center_x;
    float preview_center_y;
    uint8_t backlight_percent;
    uint8_t light_percent;
    uint8_t screen_timeout_min;
    uint32_t config_generation;
    int jpeg_live_frames;
    int jpeg_record_frames;
    int network_link;
    char network_ip[64];
    uint8_t storage_mounted;
    uint64_t storage_available_bytes;
    uint32_t capture_fps; /* 驱动报告的采集帧率；0 表示驱动未返回有效值 */
    uint32_t output_fps;  /* 依据目标/软件选帧推导的输出上限，实测值见 metrics 日志 */
    uint64_t jpeg_live_dropped;
    uint64_t jpeg_record_dropped;
    uint64_t capture_frames;
    uint64_t capture_dropped_display;
    uint64_t capture_dropped_encode;
    ipcam_record_status_t record;
} ipcam_control_status_t;

typedef enum ipcam_control_command_type_e {
    IPCAM_CONTROL_SET_VIDEO = 1,
    IPCAM_CONTROL_SET_MIRROR,
    IPCAM_CONTROL_SET_PREVIEW,
    IPCAM_CONTROL_SET_VIEW,
    IPCAM_CONTROL_SET_BACKLIGHT,
    IPCAM_CONTROL_SET_LIGHT,
    IPCAM_CONTROL_SET_SCREEN_TIMEOUT,
    IPCAM_CONTROL_RECORD_START,
    IPCAM_CONTROL_RECORD_STOP,
    IPCAM_CONTROL_PHOTO
} ipcam_control_command_type_t;

typedef struct ipcam_control_command_s {
    ipcam_control_command_type_t type;
    ipcam_control_video_t video;
    int enabled;
    int mirror_horizontal;
    int mirror_vertical;
    float zoom;
    float center_x;
    float center_y;
    int percent;
    int timeout_min;
} ipcam_control_command_t;

typedef enum ipcam_control_result_state_e {
    IPCAM_CONTROL_RESULT_PROCESSING = 0,
    IPCAM_CONTROL_RESULT_DONE,
    IPCAM_CONTROL_RESULT_FAILED
} ipcam_control_result_state_t;

typedef struct ipcam_control_result_s {
    uint64_t request_id;
    ipcam_control_result_state_t state;
    int persisted; /* 1 表示涉及的持久字段已成功落盘 */
    int error_code;
    char message[128];
    char path[256]; /* 拍照成功时返回正式文件路径 */
} ipcam_control_result_t;

typedef struct ipcam_control_ctx_s {
    ipcam_record_ctx_t *recorder;
    ipcam_capture_ctx_t *capture;
    ipcam_display_ctx_t *display;
    ipcam_screen_ctx_t *screen;
    ipcam_ring_buffer_t *jpeg_live_rb;
    volatile sig_atomic_t *running;
    pthread_mutex_t mtx;
    uint64_t next_request_id;
    ipcam_control_result_t results[16];
    size_t result_cursor;
    int initialized;
} ipcam_control_ctx_t;

/* 初始化控制队列；调用方须保证引用的服务在控制器销毁前仍有效。 */
int ipcam_control_init(ipcam_control_ctx_t *ctx,
                       ipcam_capture_ctx_t *capture,
                       ipcam_record_ctx_t *recorder,
                       ipcam_display_ctx_t *display,
                       ipcam_screen_ctx_t *screen,
                       ipcam_ring_buffer_t *jpeg_live_rb,
                       volatile sig_atomic_t *running);
void ipcam_control_deinit(ipcam_control_ctx_t *ctx);

/* 返回已验证能力和当前真实状态；函数只读，不触发媒体切换。 */
int ipcam_control_get_capabilities(ipcam_control_ctx_t *ctx,
                                   ipcam_control_capabilities_t *out);
int ipcam_control_get_status(ipcam_control_ctx_t *ctx,
                             ipcam_control_status_t *out);

/* 串行校验并提交一个命令，request_id 可用于查询最终结果。 */
int ipcam_control_submit_command(ipcam_control_ctx_t *ctx,
                                 const ipcam_control_command_t *command,
                                 uint64_t *request_id);
int ipcam_control_get_command_result(ipcam_control_ctx_t *ctx,
                                     uint64_t request_id,
                                     ipcam_control_result_t *out);

/* 预览帧采用“复制即移交”语义；release 当前为对称空操作，保留扩展点。 */
int ipcam_control_preview_acquire(ipcam_control_ctx_t *ctx,
                                  void *out_data, size_t out_cap,
                                  ipcam_frame_t *out_frame);
void ipcam_control_preview_release(ipcam_control_ctx_t *ctx,
                                   ipcam_frame_t *frame);

#endif /* IPCAM_CONTROL_H */
