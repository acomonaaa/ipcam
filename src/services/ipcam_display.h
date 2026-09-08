#ifndef IPCAM_DISPLAY_H
#define IPCAM_DISPLAY_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include <stddef.h>     /* size_t */
#include <linux/fb.h>   /* fb_var_screeninfo */
#include "ipcam_frame_diag.h"
#include "ipcam_ringbuffer.h"

/*
 * LCD 显示线程：从环形缓冲读 YUYV 帧，转换为 RGB565，写入 /dev/fb0 mmap。
 * 颜色转换用软件算法（参考 11/19_lcd/lcd_test.c 的 argb8888_to_rgb565 宏风格）。
 *
 * 优先使用 mxsfb 的双页 framebuffer：完整帧写入后台页后通过
 * FBIOPAN_DISPLAY 在 VSYNC 时翻页，避免 LCD 扫描过程中读到半帧。
 * 双页不可用时退化为临时缓冲区 + FBIO_WAITFORVSYNC，启动日志会明确标注。
 * 当前仅支持 RGB565 16bpp framebuffer；其他 bpp 在启动时返回 -1。
 */

typedef struct ipcam_display_ctx_s {
    int      fb_fd;
    int      fb_w;
    int      fb_h;
    int      fb_bpp;
    int      fb_line_length;    /* finfo.line_length；0 表示未设置 */
    unsigned short *fb_base;    /* mmap 后的帧缓冲基址（RGB565 16bpp） */
    size_t   fb_size;
    size_t   fb_page_bytes;     /* 一页可见区域的字节数 */
    unsigned int fb_yres_virtual;
    int      fb_page_count;     /* 2=硬件双页，1=单页退化 */
    int      fb_current_page;
    int      fb_flip_enabled;
    int      fb_vsync_available; /* -1 未探测，0 不支持，1 支持 */
    struct fb_var_screeninfo fb_var;
    struct fb_fix_screeninfo fb_fix;
    unsigned short *fb_staging;  /* 单页退化路径的完整帧临时缓冲 */
    size_t   fb_staging_size;

    /* 显示诊断：每个物理页保留上一次写入后的指纹，复用前检查是否被改写。 */
    ipcam_frame_probe_t fb_page_probe[2];
    unsigned char fb_page_probe_valid[2];
    unsigned long ring_hash_mismatch;
    unsigned long fb_page_corrupt;
    unsigned long fb_layout_changes;
    unsigned long pan_errors;
    unsigned long pan_timeouts;
    unsigned long unexpected_yoffset;
    unsigned long fb_query_errors;
    unsigned long fb_probe_errors;
    unsigned long frame_probe_errors;
    unsigned long source_sequence_gaps;
    unsigned long source_sequence_rewinds;
    unsigned long pan_calls;
    unsigned long pan_wait_over_threshold;
    unsigned long long pan_total_us;
    unsigned long long pan_max_us;
    unsigned long long pan_min_us;
    uint64_t last_source_sequence;
    uint32_t last_source_probe;
    uint32_t last_rgb_probe;
    int have_source_sequence;

    /* 目标显示尺寸（一般是 LCD 全屏） */
    int      out_w;
    int      out_h;

    /* 协商后的源 YUYV 尺寸（capture 传入） */
    int      src_w;
    int      src_h;

    ipcam_ring_buffer_t *rb;    /* 读取端 */
    volatile sig_atomic_t *running;
    pthread_t  thread;
} ipcam_display_ctx_t;

int  ipcam_display_start(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                         int src_w, int src_h,
                         volatile sig_atomic_t *running);
void ipcam_display_stop(ipcam_display_ctx_t *ctx);

#endif /* IPCAM_DISPLAY_H */
