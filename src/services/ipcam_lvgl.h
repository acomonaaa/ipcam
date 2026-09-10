#ifndef IPCAM_LVGL_H
#define IPCAM_LVGL_H

/*
 * LVGL 9 Linux 适配服务。
 *
 * 显示设备复用 ipcam_display 已打开并映射的 RGB565 framebuffer，LVGL 负责
 * UI 合成和局部刷新；触摸设备仍由 ipcam_touch 的唯一 evdev 线程解析，再以
 * 快照形式交给本服务，避免同一个 /dev/input/event1 被两个线程抢读。
 */

#include <lvgl.h>

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "ipcam_control.h"
#include "ipcam_display.h"
#include "ipcam_perf.h"
#include "ipcam_touch.h"
#include "ipcam_ui.h"

#define IPCAM_LVGL_ACTION_QUEUE_DEPTH 32

typedef struct ipcam_lvgl_perf_s {
    uint64_t handler_avg_ns;
    uint64_t handler_p95_ns;
    uint64_t handler_max_ns;
    uint64_t video_avg_ns;
    uint64_t video_p95_ns;
    uint64_t video_max_ns;
    uint64_t sleep_avg_ns;
    uint64_t sleep_p95_ns;
    uint64_t sleep_max_ns;
    uint64_t handler_count;
    uint64_t video_frames;
    uint64_t sleep_redraws;
    int sleep_fast_path;
    int sleep_fast_active;
} ipcam_lvgl_perf_t;

typedef struct ipcam_lvgl_ctx_s {
    ipcam_display_ctx_t *display;
    volatile sig_atomic_t *running;
    volatile sig_atomic_t service_running;
    pthread_t thread;
    int thread_started;

    lv_display_t *lv_display;
    lv_indev_t *lv_indev;
    uint8_t *draw_buf;
    size_t draw_buf_size;
    int framebuffer_direct;
    uint8_t *video_buf;
    size_t video_buf_size;
    lv_image_dsc_t video_dsc;
    ipcam_ui_t *ui;

    /* 休眠提示的固定合成缓冲；边沿时生成 backdrop，视频帧只更新同矩形图层。 */
    uint16_t *sleep_bg_buf;
    size_t sleep_bg_size;
    uint16_t *sleep_video_buf;
    size_t sleep_video_size;
    uint16_t *sleep_lut;
    lv_image_dsc_t sleep_bg_dsc;
    lv_image_dsc_t sleep_video_dsc;
    int sleep_fast_path;
    int sleep_fast_active;
    uint64_t sleep_prompt_redraws;

    uint64_t last_rendered;
    uint64_t last_fps_frames;
    uint32_t last_fps_tick;
    uint32_t last_ui_tick;
    float measured_fps;
    int video_valid;
    int preview_target_w;
    int preview_target_h;
    ipcam_ui_screen_t preview_target_screen;

    /* 触摸线程只写快照；LVGL read_cb 在 LVGL 线程中读取这组数据。 */
    pthread_mutex_t input_mtx;
    int touch_pressed;
    int touch_x;
    int touch_y;

    /* UI 回调只入队；主线程在 LVGL 线程之外提交 control 命令。 */
    pthread_mutex_t action_mtx;
    ipcam_ui_action_t action_queue[IPCAM_LVGL_ACTION_QUEUE_DEPTH];
    size_t action_head;
    size_t action_count;
    /* 记录尚未完成的开始/停止请求，避免主线程每秒轮询前 UI 又回到旧状态。 */
    ipcam_ui_action_type_t pending_record_action;
    /* 拍照会等待下一张 JPEG；等待期间拒绝重复请求，避免动作队列逐渐堆满。 */
    int photo_inflight;

    /* control 在 LVGL 启动后才创建，用锁保护指针发布和服务退出顺序。 */
    pthread_mutex_t service_mtx;
    ipcam_control_ctx_t *control;

    pthread_mutex_t stats_mtx; /* 保护 handler/video/sleep 固定窗口 */
    ipcam_perf_window_t handler_window;
    ipcam_perf_window_t video_window;
    ipcam_perf_window_t sleep_window;
    uint64_t handler_count;
    uint64_t video_frames;

    /* 主线程不能直接调用 LVGL；动作结果先放入反馈槽，由 LVGL 线程在
     * 下一次状态刷新时显示。格式化期间单独保留忙状态，防止重复提交。 */
    pthread_mutex_t feedback_mtx;
    char feedback[128];
    ipcam_ui_message_severity_t feedback_severity;
    int storage_formatting;
} ipcam_lvgl_ctx_t;

/* 复用已经打开的 framebuffer，创建 LVGL 9 显示、输入和 IPCam 设计页面。 */
int ipcam_lvgl_start(ipcam_lvgl_ctx_t *ctx, ipcam_display_ctx_t *display,
                     volatile sig_atomic_t *running);

/* 在 control 初始化完成后发布控制句柄，UI 线程随后开始显示真实业务状态。 */
void ipcam_lvgl_set_control(ipcam_lvgl_ctx_t *ctx, ipcam_control_ctx_t *control);

/* 主线程周期调用，取出 UI 动作并转换为统一 control 命令。 */
void ipcam_lvgl_process_actions(ipcam_lvgl_ctx_t *ctx);

/* 接收 ipcam_touch 在 SYN_REPORT 时产生的像素坐标触点快照。 */
void ipcam_lvgl_touch_report(ipcam_lvgl_ctx_t *ctx,
                             const ipcam_touch_point_t *points, int count);

/* 复制 LVGL 渲染统计；不触碰 UI 对象、网络或存储状态。 */
void ipcam_lvgl_get_perf(ipcam_lvgl_ctx_t *ctx, ipcam_lvgl_perf_t *out);

/* 停止 LVGL 主循环并释放其对象；调用时触摸服务必须已经停止。 */
void ipcam_lvgl_stop(ipcam_lvgl_ctx_t *ctx);

#endif /* IPCAM_LVGL_H */
