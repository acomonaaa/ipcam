#define _GNU_SOURCE
/* JPEG 转码与输出队列诊断归入 ENC 模块，便于串口按业务筛选。 */
#define IPCAM_LOG_MODULE "ENC "
#include "ipcam_encode.h"
#include "ipcam_log.h"
#include "ipcam_param.h"
#include "ipcam_yuyv.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <turbojpeg.h>
#include <unistd.h>

#include "ipcam_config.h"

/* 统一读取单调时钟；媒体时延不使用会被校时影响的 gettimeofday。 */
static uint64_t encode_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * 质量开关只在服务启动时读取，运行过程中允许配置质量改变但不允许
 * 环境变量被其它线程动态改写；这使状态机的行为可复现且不增加每帧解析。
 */
static int encode_env_adaptive_enabled(void)
{
    const char *value = getenv("IPCAM_JPEG_ADAPTIVE");
    if (!value || !*value) return 1;
    return strcmp(value, "0") != 0;
}

/* 记录跳过/输入陈旧事件；只操作统计锁，不触碰媒体 ring。 */
static void encode_add_dropped(ipcam_encode_ctx_t *ctx, uint64_t count)
{
    if (!ctx || count == 0) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    ctx->frames_dropped += count;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

static void encode_add_stale(ipcam_encode_ctx_t *ctx, unsigned int count)
{
    if (!ctx || count == 0) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    ctx->stale_input_frames += count;
    ctx->window_stale_frames += count;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/* 获取当前有效质量；质量控制器只由编码线程更新，查询仍通过统计锁同步。 */
static int encode_get_quality(ipcam_encode_ctx_t *ctx)
{
    int quality = IPCAM_JPEG_QUALITY;
    pthread_mutex_lock(&ctx->stats_mtx);
    quality = ctx->quality > 0 ? ctx->quality : IPCAM_JPEG_QUALITY;
    pthread_mutex_unlock(&ctx->stats_mtx);
    return quality;
}

/* 配置通过 HTTP/界面改变后，下一帧前把控制器重新锚定到新配置质量。 */
static void encode_refresh_quality_config(ipcam_encode_ctx_t *ctx)
{
    uint8_t configured = ipcam_param_get_jpeg_quality();
    pthread_mutex_lock(&ctx->stats_mtx);
    if (configured != ctx->configured_quality) {
        if (ipcam_quality_reconfigure(&ctx->quality_controller, configured,
                                      ctx->adaptive_quality) >= 0) {
            ctx->configured_quality = configured;
            ctx->quality = ctx->quality_controller.effective;
        }
    }
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/* 维护一秒窗口，并在窗口边界执行一次质量降档/恢复决策。 */
static void encode_add_sample(ipcam_encode_ctx_t *ctx, uint64_t encode_ns,
                              uint64_t latency_ns, uint64_t bytes,
                              uint64_t yuv420_ns, uint64_t jpeg_ns)
{
    uint64_t now = encode_now_ns();
    int quality_changed = 0;
    int effective_quality = 0;
    if (!now) return;

    pthread_mutex_lock(&ctx->stats_mtx);
    ctx->frames_encoded++;
    ctx->bytes_encoded += bytes;
    ipcam_perf_window_add(&ctx->encode_window, encode_ns);
    ipcam_perf_window_add(&ctx->yuv420_window, yuv420_ns);
    ipcam_perf_window_add(&ctx->jpeg_window, jpeg_ns);
    ipcam_perf_window_add(&ctx->jpeg_bytes_window, bytes);
    ipcam_perf_window_add(&ctx->latency_window, latency_ns);
    if (ctx->window_started_ns == 0) ctx->window_started_ns = now;

    if (now >= ctx->window_started_ns &&
        now - ctx->window_started_ns >= 1000000000ULL) {
        uint64_t frame_budget = 1000000000ULL /
                                (ctx->target_fps ? ctx->target_fps : 15U);
        ctx->last_window_perf.encode_avg_ns = ipcam_perf_window_avg(&ctx->encode_window);
        ctx->last_window_perf.encode_p95_ns = ipcam_perf_window_p95(&ctx->encode_window);
        ctx->last_window_perf.encode_max_ns = ipcam_perf_window_max(&ctx->encode_window);
        ctx->last_window_perf.yuv420_avg_ns = ipcam_perf_window_avg(&ctx->yuv420_window);
        ctx->last_window_perf.yuv420_p95_ns = ipcam_perf_window_p95(&ctx->yuv420_window);
        ctx->last_window_perf.yuv420_max_ns = ipcam_perf_window_max(&ctx->yuv420_window);
        ctx->last_window_perf.jpeg_avg_ns = ipcam_perf_window_avg(&ctx->jpeg_window);
        ctx->last_window_perf.jpeg_p95_ns = ipcam_perf_window_p95(&ctx->jpeg_window);
        ctx->last_window_perf.jpeg_max_ns = ipcam_perf_window_max(&ctx->jpeg_window);
        ctx->last_window_perf.jpeg_avg_bytes = ipcam_perf_window_avg(&ctx->jpeg_bytes_window);
        ctx->last_window_perf.jpeg_max_bytes = ipcam_perf_window_max(&ctx->jpeg_bytes_window);
        ctx->last_window_perf.capture_to_output_p95_ns =
            ipcam_perf_window_p95(&ctx->latency_window);
        ctx->last_window_perf.window_frames =
            (uint32_t)ipcam_perf_window_count(&ctx->encode_window);
        quality_changed = ipcam_quality_update(
            &ctx->quality_controller, ctx->window_stale_frames != 0,
            ctx->last_window_perf.encode_p95_ns,
            ctx->last_window_perf.capture_to_output_p95_ns, frame_budget);
        effective_quality = ctx->quality_controller.effective;
        ctx->quality = effective_quality;
        ctx->last_window_perf.configured_quality = ctx->configured_quality;
        ctx->last_window_perf.effective_quality = (uint8_t)effective_quality;
        ctx->last_window_perf.adaptive_quality = ctx->adaptive_quality;
        ipcam_perf_window_reset(&ctx->encode_window);
        ipcam_perf_window_reset(&ctx->yuv420_window);
        ipcam_perf_window_reset(&ctx->jpeg_window);
        ipcam_perf_window_reset(&ctx->jpeg_bytes_window);
        ipcam_perf_window_reset(&ctx->latency_window);
        ctx->window_stale_frames = 0;
        ctx->window_started_ns = now;
    }
    pthread_mutex_unlock(&ctx->stats_mtx);

    if (quality_changed) {
        MLOGI("adaptive jpeg quality changed: configured=%u effective=%d\n",
              ctx->configured_quality, effective_quality);
    }
}

static void encode_add_output_counters(ipcam_encode_ctx_t *ctx,
                                       int live_overwrite, int record_drop)
{
    pthread_mutex_lock(&ctx->stats_mtx);
    if (live_overwrite) ctx->live_overwrites++;
    if (record_drop) ctx->record_drops++;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/* 单次 JPEG 编码后广播到直播最新帧槽和录像有界队列，慢录像盘只丢帧不反压。 */
static void *encode_thread(void *arg)
{
    ipcam_encode_ctx_t *ctx = arg;
    ipcam_frame_t frame;
    unsigned long encoded = 0, skipped = 0;
    unsigned long record_drops = 0;
    uint64_t thread_started_ns = encode_now_ns();

    tjhandle tj = tjInitCompress();
    if (!tj) {
        MLOGE("tjInitCompress: %s\n", tjGetErrorStr());
        return NULL;
    }

    /* 启动日志打印持久化配置和自适应开关，现场可直接确认 q60 是否生效。 */
    MLOGI("encode thread start, configured_quality=%u effective_quality=%d adaptive=%d w=%d h=%d\n",
          ctx->configured_quality, encode_get_quality(ctx), ctx->adaptive_quality,
          ctx->width, ctx->height);

    int W = ctx->width;
    int H = ctx->height;
    int Wp = W / 2;       /* pair width */
    int Ch = (H + 1) / 2; /* TJSAMP_420 的色度平面高度 */
    MLOGI("JPEG pipeline ready: samp=420 y=%zu chroma=%zu/%zu\n",
          (size_t)W * (size_t)H, (size_t)Wp * (size_t)Ch,
          (size_t)Wp * (size_t)Ch);

    /* planar 工作缓冲：每次循环复用，避免每帧 malloc */
    unsigned char *Yp  = malloc((size_t)W * H);
    unsigned char *Cbp = malloc((size_t)Wp * Ch);
    unsigned char *Crp = malloc((size_t)Wp * Ch);
    if (!Yp || !Cbp || !Crp) {
        MLOGE("alloc YUV planes failed\n");
        free(Yp); free(Cbp); free(Crp);
        tjDestroy(tj);
        return NULL;
    }

    unsigned char *jpeg_buf = tjAlloc(ctx->jpeg_capacity);
    if (!jpeg_buf) {
        MLOGE("tjAlloc jpeg buffer failed, capacity=%lu\n", ctx->jpeg_capacity);
        free(Yp); free(Cbp); free(Crp);
        tjDestroy(tj);
        return NULL;
    }

    while (*ctx->running && ctx->service_running) {
        unsigned int stale_count = 0;
        /* 编码速度低于采集速度时主动丢弃旧帧，避免延迟随时间线性增长；
         * 定时接口同时保证 stop/close 能唤醒，不需要额外 sleep 轮询。 */
        int get_rc = ipcam_ring_get_latest_ex(ctx->in_rb, &frame, &stale_count, 1000);
        if (get_rc == 1) continue;
        if (get_rc != 0) break;
        encode_add_stale(ctx, stale_count);
        encode_refresh_quality_config(ctx);

        size_t stride = frame.stride ? frame.stride : (size_t)W * 2;
        if (stride < (size_t)W * 2 || frame.size < stride * (size_t)H || (W & 1)) {
            MLOGW("frame size %zu/stride %zu invalid for %dx%d, skip\n",
                  frame.size, stride, W, H);
            skipped++;
            encode_add_dropped(ctx, 1);
            ipcam_ring_release(ctx->in_rb);
            continue;
        }

        uint64_t encode_started_ns = encode_now_ns();
        if (ipcam_yuyv_to_yuv420((const uint8_t *)frame.rawData, W, H,
                                 (int)stride, Yp, W, Cbp, Wp, Crp, Wp,
                                 ipcam_param_get_mirror_horizontal(),
                                 ipcam_param_get_mirror_vertical()) != 0) {
            MLOGW("YUYV to YUV420 conversion failed for %dx%d stride=%zu\n",
                  W, H, stride);
            skipped++;
            encode_add_dropped(ctx, 1);
            ipcam_ring_release(ctx->in_rb);
            continue;
        }

        uint64_t yuv420_finished_ns = encode_now_ns();
        uint64_t jpeg_started_ns = yuv420_finished_ns;

        const unsigned char *planes[3] = { Yp, Cbp, Crp };
        int strides[3] = { W, Wp, Wp };
        unsigned long jpeg_size = ctx->jpeg_capacity;
        int quality = encode_get_quality(ctx);

        /*
         * libjpeg-turbo 2.1.x 参数顺序为 (handle, planes, width, strides, height, …)。
         * 曾误写成 strides/W 对调，会导致压缩失败、崩溃或垃圾 JPEG。
         */
        if (tjCompressFromYUVPlanes(tj, planes, W, strides, H, TJSAMP_420,
                                    &jpeg_buf, &jpeg_size,
                                    quality,
                                    TJFLAG_NOREALLOC | TJFLAG_FASTDCT) != 0) {
            MLOGW("tjCompressFromYUVPlanes: %s\n", tjGetErrorStr2(tj));
            skipped++;
            encode_add_dropped(ctx, 1);
            ipcam_ring_release(ctx->in_rb);
            continue;
        }

        uint64_t jpeg_finished_ns = encode_now_ns();

        ipcam_frame_meta_t out_meta;
        memset(&out_meta, 0, sizeof(out_meta));
        out_meta.monotonic_ns = frame.monotonic_ns;
        out_meta.width = (uint16_t)W;
        out_meta.height = (uint16_t)H;
        out_meta.config_generation = frame.config_generation;
        out_meta.quality = (uint8_t)quality;
        int live_rc = ipcam_ring_try_append_latest_meta(ctx->out_rb, jpeg_buf,
                                                        jpeg_size, &out_meta);
        if (live_rc > 0) {
            /* 覆盖旧直播帧是低延迟策略的正常结果，不应算作编码失败。 */
        }
        if (live_rc < 0) {
            /* ring 关闭通常发生在停机阶段；仅记录输出失败，不污染编码丢帧统计。 */
            MLOGW("live jpeg ring unavailable while encoding\n");
        }
        encoded++;
        int record_rc = 0;
        if (ctx->aux_rb) {
            record_rc = ipcam_ring_try_append_meta(ctx->aux_rb, jpeg_buf, jpeg_size, &out_meta);
            if (record_rc != 0) {
                record_drops++;
                if ((record_drops % 30) == 1) MLOGW("record jpeg ring full, dropping frame\n");
            }
        }

        encode_add_output_counters(ctx, live_rc > 0, record_rc != 0);
        uint64_t output_ns = encode_now_ns();
        encode_add_sample(ctx, output_ns > encode_started_ns ? output_ns - encode_started_ns : 0,
                          output_ns > frame.monotonic_ns ? output_ns - frame.monotonic_ns : 0,
                          jpeg_size,
                          yuv420_finished_ns > encode_started_ns ?
                              yuv420_finished_ns - encode_started_ns : 0,
                          jpeg_finished_ns > jpeg_started_ns ?
                              jpeg_finished_ns - jpeg_started_ns : 0);

        ipcam_ring_release(ctx->in_rb);
        /*
         * BCF2 的视频线程不会只在退出时给结果，而是打印首批帧和周期帧。
         * 这里记录输入序号、JPEG 大小、时间戳以及两个消费者的结果，能直接
         * 判断是编码慢、直播覆盖还是录像队列溢出，同时限制串口输出频率。
         */
        if (encoded <= 3) {
            MLOGI("frame no=%lu src_seq=%lu ts=%llu jpeg=%lu live_rc=%d record_rc=%d\n",
                  encoded, frame.seqNo, (unsigned long long)frame.monotonic_ns,
                  jpeg_size, live_rc, record_rc);
        }
    }

    free(Yp); free(Cbp); free(Crp);
    tjFree(jpeg_buf);
    tjDestroy(tj);

    uint64_t thread_ended_ns = encode_now_ns();
    double sec = thread_ended_ns > thread_started_ns ?
                 (double)(thread_ended_ns - thread_started_ns) / 1e9 : 0.0;
    MLOGI("encode thread exit, encoded=%lu skipped=%lu live_overwrites=%llu record_drops=%llu avg_fps=%.1f\n",
          encoded, skipped,
          (unsigned long long)ctx->live_overwrites,
          (unsigned long long)ctx->record_drops,
          sec > 0 ? encoded / sec : 0.0);
    return NULL;
}

/* 启动编码线程；aux 为可选录像队列，NULL 时保持原直播用法。 */
int ipcam_encode_start_ex(ipcam_encode_ctx_t *ctx,
                          ipcam_ring_buffer_t *in,
                          ipcam_ring_buffer_t *out,
                          ipcam_ring_buffer_t *aux,
                          int src_w, int src_h,
                          volatile sig_atomic_t *running)
{
    if (!ctx || !in || !out || !running) return -1;
    int width = src_w > 0 ? src_w : IPCAM_CAPTURE_WIDTH;
    int height = src_h > 0 ? src_h : IPCAM_CAPTURE_HEIGHT;
    if (width <= 0 || height <= 0 || (width & 1)) {
        MLOGE("invalid YUYV dimensions %dx%d\n", width, height);
        return -1;
    }

    /* tjBufSize 是 4:2:0 的理论最大 JPEG 尺寸；启动时一次校验所有输出
     * ring，运行中使用 TJFLAG_NOREALLOC，避免编码器隐藏 realloc 造成抖动。 */
    unsigned long jpeg_capacity = tjBufSize(width, height, TJSAMP_420);
    if (jpeg_capacity == 0 || ipcam_ring_capacity(out) < jpeg_capacity ||
        (aux && ipcam_ring_capacity(aux) < jpeg_capacity)) {
        MLOGE("JPEG ring capacity insufficient: need=%lu live=%zu record=%zu\n",
              jpeg_capacity, ipcam_ring_capacity(out),
              aux ? ipcam_ring_capacity(aux) : 0U);
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->in_rb = in;
    ctx->out_rb = out;
    ctx->aux_rb = aux;
    ctx->running = running;
    ctx->service_running = 1;
    pthread_mutex_init(&ctx->stats_mtx, NULL);
    ctx->width  = width;
    ctx->height = height;
    ctx->target_fps = ipcam_param_get_target_fps();
    ctx->jpeg_capacity = jpeg_capacity;
    ctx->configured_quality = ipcam_param_get_jpeg_quality();
    ctx->adaptive_quality = (uint8_t)encode_env_adaptive_enabled();
    if (ipcam_quality_init(&ctx->quality_controller, ctx->configured_quality,
                           ctx->adaptive_quality) != 0) {
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    ctx->quality = ctx->quality_controller.effective;
    ipcam_perf_window_init(&ctx->encode_window);
    ipcam_perf_window_init(&ctx->yuv420_window);
    ipcam_perf_window_init(&ctx->jpeg_window);
    ipcam_perf_window_init(&ctx->jpeg_bytes_window);
    ipcam_perf_window_init(&ctx->latency_window);
    ctx->last_window_perf.configured_quality = ctx->configured_quality;
    ctx->last_window_perf.effective_quality = (uint8_t)ctx->quality;
    ctx->last_window_perf.adaptive_quality = ctx->adaptive_quality;

    if (pthread_create(&ctx->thread, NULL, encode_thread, ctx) != 0) {
        MLOGE("pthread_create encode failed\n");
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    MLOGI("encode service ready: input=%dx%d quality=%d adaptive=%d jpeg_capacity=%lu live_rb=%s record_rb=%s\n",
          ctx->width, ctx->height, ctx->quality, ctx->adaptive_quality,
          ctx->jpeg_capacity,
          ctx->out_rb ? "yes" : "no", ctx->aux_rb ? "yes" : "no");
    return 0;
}

/* 关闭编码相关环槽并等待线程退出，不修改采集或主进程运行标志。 */
void ipcam_encode_stop(ipcam_encode_ctx_t *ctx)
{
    if (!ctx) return;
    uint64_t encoded = 0, dropped = 0;
    ipcam_encode_get_stats(ctx, &encoded, &dropped);
    MLOGI("encode stop requested: encoded=%llu dropped=%llu\n",
          (unsigned long long)encoded, (unsigned long long)dropped);
    ctx->service_running = 0;
    /* 仅关闭编码服务自己的输入和输出，避免修改其它线程的运行状态。 */
    if (ctx->in_rb) ipcam_ring_close(ctx->in_rb);
    if (ctx->out_rb) ipcam_ring_close(ctx->out_rb);
    if (ctx->aux_rb) ipcam_ring_close(ctx->aux_rb);

    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }
    pthread_mutex_destroy(&ctx->stats_mtx);
    MLOGI("encode stopped\n");
}

/* 兼容旧接口：不挂接录像队列。 */
int ipcam_encode_start(ipcam_encode_ctx_t *ctx,
                       ipcam_ring_buffer_t *in,
                       ipcam_ring_buffer_t *out,
                       int src_w, int src_h,
                       volatile sig_atomic_t *running)
{
    return ipcam_encode_start_ex(ctx, in, out, NULL, src_w, src_h, running);
}

/* 复制编码统计快照；调用方无需持有编码线程锁。 */
void ipcam_encode_get_stats(ipcam_encode_ctx_t *ctx, uint64_t *encoded,
                            uint64_t *dropped)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    if (encoded) *encoded = ctx->frames_encoded;
    if (dropped) *dropped = ctx->frames_dropped;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/*
 * 复制编码统计快照；当前窗口尚未跨过一秒时，使用正在收集的固定数组，
 * 让 /api/status 能看到启动初期的真实数据，同时不在查询线程做任何探测。
 */
void ipcam_encode_get_perf(ipcam_encode_ctx_t *ctx, ipcam_encode_perf_t *out)
{
    if (!ctx || !out) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    *out = ctx->last_window_perf;
    out->frames_encoded = ctx->frames_encoded;
    out->frames_dropped = ctx->frames_dropped;
    out->stale_input_frames = ctx->stale_input_frames;
    out->live_overwrites = ctx->live_overwrites;
    out->record_drops = ctx->record_drops;
    out->bytes_encoded = ctx->bytes_encoded;
    out->configured_quality = ctx->configured_quality;
    out->effective_quality = (uint8_t)ctx->quality;
    out->adaptive_quality = ctx->adaptive_quality;

    if (ipcam_perf_window_count(&ctx->encode_window) > 0) {
        out->encode_avg_ns = ipcam_perf_window_avg(&ctx->encode_window);
        out->encode_p95_ns = ipcam_perf_window_p95(&ctx->encode_window);
        out->encode_max_ns = ipcam_perf_window_max(&ctx->encode_window);
        out->yuv420_avg_ns = ipcam_perf_window_avg(&ctx->yuv420_window);
        out->yuv420_p95_ns = ipcam_perf_window_p95(&ctx->yuv420_window);
        out->yuv420_max_ns = ipcam_perf_window_max(&ctx->yuv420_window);
        out->jpeg_avg_ns = ipcam_perf_window_avg(&ctx->jpeg_window);
        out->jpeg_p95_ns = ipcam_perf_window_p95(&ctx->jpeg_window);
        out->jpeg_max_ns = ipcam_perf_window_max(&ctx->jpeg_window);
        out->jpeg_avg_bytes = ipcam_perf_window_avg(&ctx->jpeg_bytes_window);
        out->jpeg_max_bytes = ipcam_perf_window_max(&ctx->jpeg_bytes_window);
        out->capture_to_output_p95_ns = ipcam_perf_window_p95(&ctx->latency_window);
        out->window_frames = (uint32_t)ipcam_perf_window_count(&ctx->encode_window);
    }
    pthread_mutex_unlock(&ctx->stats_mtx);
}
