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
    int                  width;      /* 协商后实际宽度（采集） */
    int                  height;     /* 协商后实际高度（采集） */
    int                  out_w;      /* 编码输出宽（0 传入时 = width） */
    int                  out_h;      /* 编码输出高（0 传入时 = height） */
    int                  scale_on;   /* 1 = 输出 != 采集，需逐平面插值缩放 */
    unsigned char       *Ys;         /* 缩放后 Y 平面（scale_on 时分配） */
    unsigned char       *Cbs;        /* 缩放后 Cb 平面（scale_on 时分配） */
    unsigned char       *Crs;        /* 缩放后 Cr 平面（scale_on 时分配） */
    int                 *xs0;        /* 列映射左邻索引缓存（scale_on 时分配） */
    int                 *xfrac;      /* 列映射小数权重缓存（scale_on 时分配） */
    ipcam_ring_buffer_t *in_rb;      /* 输入：packed 4:2:2 */
    ipcam_ring_buffer_t *out_rb;     /* 输出：JPEG 字节流 */
    volatile sig_atomic_t *running;
    int                  quality;    /* 1..100 */

    pthread_t            thread;
} ipcam_encode_ctx_t;

/*
 * src_w/src_h：采集协商分辨率；out_w/out_h：输出（交付）分辨率，
 * 传 0 表示跟随采集分辨率（旁路缩放）。out_w 必须为偶数。
 */
int  ipcam_encode_start(ipcam_encode_ctx_t *ctx,
                        ipcam_ring_buffer_t *in,
                        ipcam_ring_buffer_t *out,
                        int src_w, int src_h,
                        int out_w, int out_h,
                        volatile sig_atomic_t *running);
void ipcam_encode_stop(ipcam_encode_ctx_t *ctx);

#endif /* IPCAM_ENCODE_H */