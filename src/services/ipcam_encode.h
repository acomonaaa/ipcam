#ifndef IPCAM_ENCODE_H
#define IPCAM_ENCODE_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include <stddef.h>     /* size_t */
#include <stdint.h>
#include "ipcam_ringbuffer.h"
#include "ipcam_perf.h"
#include "ipcam_quality.h"

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

typedef struct ipcam_encode_perf_s {
    uint64_t frames_encoded;
    uint64_t frames_dropped;
    uint64_t stale_input_frames;
    uint64_t live_overwrites;
    uint64_t record_drops;
    uint64_t bytes_encoded;
    uint64_t encode_avg_ns;
    uint64_t encode_p95_ns;
    uint64_t encode_max_ns;
    uint64_t yuv420_avg_ns;
    uint64_t yuv420_p95_ns;
    uint64_t yuv420_max_ns;
    uint64_t jpeg_avg_ns;
    uint64_t jpeg_p95_ns;
    uint64_t jpeg_max_ns;
    uint64_t jpeg_avg_bytes;
    uint64_t jpeg_max_bytes;
    uint64_t capture_to_output_p95_ns;
    uint32_t window_frames;
    uint8_t configured_quality;
    uint8_t effective_quality;
    uint8_t adaptive_quality;
} ipcam_encode_perf_t;

typedef struct ipcam_encode_ctx_s {
    int                  width;      /* 协商后实际宽度 */
    int                  height;     /* 协商后实际高度 */
    uint32_t             target_fps;  /* 当前质量控制使用的目标帧率 */
    ipcam_ring_buffer_t *in_rb;      /* 输入：YUYV */
    ipcam_ring_buffer_t *out_rb;     /* 输出：HTTP 最新帧 JPEG */
    ipcam_ring_buffer_t *aux_rb;     /* 可选输出：录像专用 JPEG 队列 */
    volatile sig_atomic_t *running;
    volatile sig_atomic_t service_running; /* 仅编码服务自身的生命周期 */
    int                  quality;    /* 1..100 */
    uint8_t              configured_quality;
    uint8_t              adaptive_quality;
    unsigned long        jpeg_capacity; /* tjBufSize 上限，线程内只分配一次 */
    ipcam_quality_controller_t quality_controller;
    pthread_mutex_t      stats_mtx;  /* 保护计数、质量快照和固定窗口结果 */
    uint64_t             frames_encoded;
    uint64_t             frames_dropped;
    uint64_t             stale_input_frames;
    uint64_t             live_overwrites;
    uint64_t             record_drops;
    uint64_t             bytes_encoded;
    uint64_t             window_stale_frames;
    uint64_t             window_started_ns;
    ipcam_perf_window_t  encode_window;
    ipcam_perf_window_t  yuv420_window;
    ipcam_perf_window_t  jpeg_window;
    ipcam_perf_window_t  jpeg_bytes_window;
    ipcam_perf_window_t  latency_window;
    ipcam_encode_perf_t  last_window_perf;

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
/* 复制最近统计窗口；不触发采样或参数探测，适合状态接口直接调用。 */
void ipcam_encode_get_perf(ipcam_encode_ctx_t *ctx, ipcam_encode_perf_t *out);

#endif /* IPCAM_ENCODE_H */
