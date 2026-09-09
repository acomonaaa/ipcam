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
 * **必须** YUYV 4:2:2 输入（与 capture 协商结果一致），编码输出使用
 * TJSAMP_420，减少色度数据量和板端编码开销：
 *   Y plane  = W * H         bytes
 *   Cb plane = (W/2) * ceil(H/2) bytes
 *   Cr plane = (W/2) * ceil(H/2) bytes
 * Cb/Cr 的垂直降采样在编码线程内完成，保持 Y 平面全分辨率和帧元数据尺寸一致。
 */
#include "ipcam_ringbuffer.h"

typedef struct ipcam_encode_ctx_s {
    int                  width;      /* 协商后实际宽度 */
    int                  height;     /* 协商后实际高度 */
    ipcam_ring_buffer_t *in_rb;      /* 输入：YUYV */
    ipcam_ring_buffer_t *out_rb;     /* 输出：HTTP 最新帧 JPEG */
    ipcam_ring_buffer_t *aux_rb;     /* 可选输出：录像专用 JPEG 队列 */
    volatile sig_atomic_t *running;
    volatile sig_atomic_t service_running; /* 仅编码服务自身的生命周期 */
    int                  quality;    /* 1..100 */
    pthread_mutex_t      stats_mtx;  /* 保护已完成 JPEG 编码计数 */
    uint64_t             frames_encoded;
    uint64_t             frames_dropped;

    pthread_t            thread;
} ipcam_encode_ctx_t;

int  ipcam_encode_start(ipcam_encode_ctx_t *ctx,
                        ipcam_ring_buffer_t *in,
                        ipcam_ring_buffer_t *out,
                        int src_w, int src_h,
                        volatile sig_atomic_t *running);
int  ipcam_encode_start_ex(ipcam_encode_ctx_t *ctx,
                           ipcam_ring_buffer_t *in,
                           ipcam_ring_buffer_t *out,
                           ipcam_ring_buffer_t *aux,
                           int src_w, int src_h,
                           volatile sig_atomic_t *running);
void ipcam_encode_stop(ipcam_encode_ctx_t *ctx);
/* 读取 JPEG 编码/跳过累计值，供主循环计算实际编码帧率。 */
void ipcam_encode_get_stats(ipcam_encode_ctx_t *ctx, uint64_t *encoded,
                            uint64_t *dropped);

#endif /* IPCAM_ENCODE_H */
