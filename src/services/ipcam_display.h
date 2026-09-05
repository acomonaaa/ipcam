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
#include "ipcam_ringbuffer.h"

typedef struct ipcam_display_ctx_s {
    int      fb_fd;
    int      fb_w;
    int      fb_h;
    int      fb_bpp;
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
    pthread_t  thread;
} ipcam_display_ctx_t;

int  ipcam_display_start(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                         int src_w, int src_h,
                         volatile sig_atomic_t *running);
void ipcam_display_stop(ipcam_display_ctx_t *ctx);

#endif /* IPCAM_DISPLAY_H */