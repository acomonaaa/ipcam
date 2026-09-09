#ifndef IPCAM_LVGL_H
#define IPCAM_LVGL_H

/*
 * LVGL 9 Linux 适配服务。
 *
 * 显示设备复用 ipcam_display 已打开并映射的 RGB565 framebuffer，LVGL 只
 * 负责 UI 合成和局部刷新；触摸设备仍由 ipcam_touch 的唯一 evdev 读取线程
 * 解析，再以快照形式交给本服务，避免同一个 /dev/input/event1 被两个线程抢读。
 */

#include <lvgl.h>

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "ipcam_display.h"
#include "ipcam_touch.h"

typedef struct ipcam_lvgl_ctx_s {
    ipcam_display_ctx_t *display;
    volatile sig_atomic_t *running;
    volatile sig_atomic_t service_running;
    pthread_t thread;

    lv_display_t *lv_display;
    lv_indev_t *lv_indev;
    uint8_t *draw_buf;
    size_t draw_buf_size;

    /* LVGL 图像源使用独立副本，避免显示线程更新时修改正在渲染的像素。 */
    uint8_t *video_buf;
    size_t video_buf_size;
    lv_image_dsc_t video_dsc;
    lv_obj_t *video_image;
    lv_obj_t *touch_label;
    lv_obj_t *video_state_label;

    uint64_t last_rendered;
    int last_preview_enabled;
    int last_touch_pressed;

    /* 触摸线程只写快照；LVGL read_cb 在 LVGL 线程中读取这组数据。 */
    pthread_mutex_t input_mtx;
    int touch_pressed;
    int touch_x;
    int touch_y;
} ipcam_lvgl_ctx_t;

/* 复用已经打开的 framebuffer，创建 LVGL 9 显示、输入和验证页面。 */
int ipcam_lvgl_start(ipcam_lvgl_ctx_t *ctx, ipcam_display_ctx_t *display,
                     volatile sig_atomic_t *running);

/* 接收 ipcam_touch 在 SYN_REPORT 时产生的像素坐标触点快照。 */
void ipcam_lvgl_touch_report(ipcam_lvgl_ctx_t *ctx,
                             const ipcam_touch_point_t *points, int count);

/* 停止 LVGL 主循环并释放其对象；调用时触摸服务必须已经停止。 */
void ipcam_lvgl_stop(ipcam_lvgl_ctx_t *ctx);

#endif /* IPCAM_LVGL_H */
