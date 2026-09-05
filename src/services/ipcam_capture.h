#ifndef IPCAM_CAPTURE_H
#define IPCAM_CAPTURE_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include <stddef.h>     /* size_t */
#include "ipcam_ringbuffer.h"

/*
 * V4L2 capture thread.
 * - 打开 /dev/video0
 * - 协商 YUYV 4:2:2 @ IPCAM_CAPTURE_WIDTH x IPCAM_CAPTURE_HEIGHT
 * - 用 mmap 申请 N 个 video buffer，循环 DQBUF -> 拷贝到两条环形缓冲 -> QBUF
 *
 * 写两条 rb_yuyv_disp / rb_yuyv_enc 给 display 与 encode 各自独立消费；
 * 任意一条满则丢该路（消费者堵住了，我们不等）。
 */
#include "ipcam_ringbuffer.h"

typedef struct ipcam_capture_ctx_s {
    int              fd;             /* /dev/video0 fd */
    int              width;
    int              height;
    int              n_bufs;         /* V4L2 缓冲数量（建议 >= 3） */
    struct v4l2_buf_info {
        void         *start;
        size_t        length;
    } *bufs;

    ipcam_ring_buffer_t *rb_disp;     /* 写入端 #1（display 消费） */
    ipcam_ring_buffer_t *rb_enc;      /* 写入端 #2（encode 消费） */
    volatile sig_atomic_t *running;
    pthread_t        thread;          /* 非 detached，可 join */
} ipcam_capture_ctx_t;

/* 初始化并启动采集线程（pthread_create 后立即返回） */
int  ipcam_capture_start(ipcam_capture_ctx_t *ctx,
                         ipcam_ring_buffer_t *rb_disp,
                         ipcam_ring_buffer_t *rb_enc,
                         volatile sig_atomic_t *running);

/* 通知线程退出 + pthread_join + 释放 V4L2 资源 */
void ipcam_capture_stop(ipcam_capture_ctx_t *ctx);

/* 查询 V4L2 协商后的实际分辨率（在 capture_start 成功后调用） */
void ipcam_capture_get_dimensions(const ipcam_capture_ctx_t *ctx, int *w, int *h);

#endif /* IPCAM_CAPTURE_H */