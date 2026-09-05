#ifndef IPCAM_ENCODE_H
#define IPCAM_ENCODE_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include <stddef.h>     /* size_t */
#include "ipcam_ringbuffer.h"

/*
 * MJPEG 编码线程：从环形缓冲读 YUYV 帧，调 libjpeg-turbo 编码为 JPEG，
 * 写入"已编码环形缓冲"供 stream_http 消费。
 *
 * **必须** YUYV 4:2:2 输入（与 capture 协商结果一致）。
 * 用 tjCompressFromYUVPlanes + TJSAMP_422，planner 布局：
 *   Y plane  = W * H         bytes
 *   Cb plane = (W/2) * H     bytes
 *   Cr plane = (W/2) * H     bytes
 */
#include "ipcam_ringbuffer.h"

typedef struct ipcam_encode_ctx_s {
    int                  width;      /* 协商后实际宽度 */
    int                  height;     /* 协商后实际高度 */
    ipcam_ring_buffer_t *in_rb;      /* 输入：YUYV */
    ipcam_ring_buffer_t *out_rb;     /* 输出：JPEG 字节流 */
    volatile sig_atomic_t *running;
    int                  quality;    /* 1..100 */

    pthread_t            thread;
} ipcam_encode_ctx_t;

int  ipcam_encode_start(ipcam_encode_ctx_t *ctx,
                        ipcam_ring_buffer_t *in,
                        ipcam_ring_buffer_t *out,
                        int src_w, int src_h,
                        volatile sig_atomic_t *running);
void ipcam_encode_stop(ipcam_encode_ctx_t *ctx);

#endif /* IPCAM_ENCODE_H */