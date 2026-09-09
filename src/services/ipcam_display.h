#ifndef IPCAM_DISPLAY_H
#define IPCAM_DISPLAY_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include <stddef.h>     /* size_t */
#include "ipcam_ringbuffer.h"

/*
 * LCD 显示线程：从环形缓冲读 YUYV 帧，转换为 RGB565，写入 /dev/fb0 mmap。
 * 颜色转换用软件算法（参考 11/19_lcd/lcd_test.c 的 argb8888_to_rgb565 宏风格）。
 *
 * 当前仅支持 RGB565 16bpp framebuffer；其他 bpp 在启动时返回 -1。
 */
typedef struct ipcam_display_ctx_s {
    int      fb_fd;
    int      fb_w;
    int      fb_h;
    int      fb_bpp;
    int      fb_xoffset;
    int      fb_yoffset;
    int      fb_line_length;    /* finfo.line_length；0 表示未设置 */
    unsigned short *fb_base;    /* mmap 后的帧缓冲基址（RGB565 16bpp） */
    size_t   fb_size;

    /* 目标显示尺寸（一般是 LCD 全屏） */
    int      out_w;
    int      out_h;

    /* 协商后的源 YUYV 尺寸（capture 传入） */
    int      src_w;
    int      src_h;

    ipcam_ring_buffer_t *rb;    /* 读取端 */
    volatile sig_atomic_t *running;
    volatile sig_atomic_t service_running; /* 仅显示服务自身的生命周期 */
    pthread_t  thread;
    pthread_mutex_t view_mtx;
    pthread_mutex_t preview_mtx; /* 保护供 GUI 复制的最新 RGB565 帧 */
    pthread_mutex_t stats_mtx;    /* 保护已渲染帧累计值 */
    uint64_t       frames_rendered;
    volatile sig_atomic_t framebuffer_writer_enabled;
    unsigned short  *preview_base;
    size_t           preview_size;
    ipcam_frame_t    preview_frame;
    int              preview_valid;
    int              view_enabled;
    int              screen_paused;
    float            zoom;
    float            center_x;
    float            center_y;
} ipcam_display_ctx_t;

/* 兼容旧调用方：直接由 display 线程写 framebuffer。 */
int  ipcam_display_start(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                         int src_w, int src_h,
                         volatile sig_atomic_t *running);
/* LVGL 模式下只生成 RGB565 预览副本，把 framebuffer 写入交给 LVGL flush。 */
int  ipcam_display_start_ex(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                            int src_w, int src_h, int framebuffer_writer,
                            volatile sig_atomic_t *running);
void ipcam_display_stop(ipcam_display_ctx_t *ctx);

/* 在 LVGL 初始化失败时切回旧的直接写屏路径，避免 LCD 因 UI 能力缺失而黑屏。 */
void ipcam_display_set_framebuffer_writer(ipcam_display_ctx_t *ctx, int enabled);

/* 设置本地观察视口；zoom 限制在 1～4，中心坐标为源图像归一化坐标。 */
int ipcam_display_set_view(ipcam_display_ctx_t *ctx, int enabled,
                           float zoom, float center_x, float center_y);
/* 熄屏期间暂停 YUYV→RGB 转换但保留用户视口，唤醒时恢复原视图。 */
int ipcam_display_set_screen_paused(ipcam_display_ctx_t *ctx, int paused);
void ipcam_display_get_view(ipcam_display_ctx_t *ctx, int *enabled,
                            float *zoom, float *center_x, float *center_y);
/* 读取本地预览已渲染帧累计值，供 5 秒性能汇总计算实际帧率。 */
void ipcam_display_get_stats(ipcam_display_ctx_t *ctx, uint64_t *rendered);
/* 复制最新 RGB565 预览帧；调用方提供 out_data，成功后无需释放 ring 槽。 */
int ipcam_display_preview_acquire(ipcam_display_ctx_t *ctx,
                                  void *out_data, size_t out_cap,
                                  ipcam_frame_t *out_frame);
void ipcam_display_preview_release(ipcam_display_ctx_t *ctx,
                                   ipcam_frame_t *frame);
/* 通过 IPCAM_BACKLIGHT_PATH（亮度）和 IPCAM_BACKLIGHT_MAX（最大值）写入背光；0 用于自动熄屏。 */
int ipcam_display_set_backlight_percent(int percent);

#endif /* IPCAM_DISPLAY_H */
