#define _GNU_SOURCE
/* JPEG 转码与输出队列诊断归入 ENC 模块，便于串口按业务筛选。 */
#define IPCAM_LOG_MODULE "ENC "
#include "ipcam_encode.h"
#include "ipcam_log.h"
#include "ipcam_param.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <turbojpeg.h>
#include <unistd.h>

#include "ipcam_config.h"

/* 统计编码线程已完成的压缩帧；统计锁不与 ring/参数锁嵌套。 */
static void encode_add_stats(ipcam_encode_ctx_t *ctx, uint64_t encoded,
                             uint64_t dropped)
{
    pthread_mutex_lock(&ctx->stats_mtx);
    ctx->frames_encoded += encoded;
    ctx->frames_dropped += dropped;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/*
 * YUYV (packed 4:2:2) -> planar 4:2:0 (Y 整分辨率 + Cb/Cr 半宽半高)
 *
 * YUYV 字节布局：
 *   b0=Y0  b1=Cb0  b2=Y1  b3=Cr0  b4=Y2  b5=Cb1  b6=Y3  b7=Cr1  ...
 *
 * 输出的 Cb/Cr 每个样本对应两行输入的平均值；奇数高度时最后一行
 * 复制参与平均。这与 TJSAMP_420 的半高平面布局一致，避免只改采样
 * 枚举却仍分配/填充 4:2:2 数据造成越界或颜色错位。
 */
/*
 * 将带实际行跨度的 YUYV 帧拆为 libjpeg-turbo 所需的平面数据。
 * V4L2 允许 bytesperline 大于 width*2；此前按紧凑行读取会把 padding
 * 当成下一行开头，导致预览和 JPEG 在部分驱动上错行，因此 stride 必须
 * 从帧元数据传入并参与每一行地址计算。
 */
static void unpack_yuyv_to_planar(const unsigned char *src, int w, int h,
                                 size_t stride, unsigned char *Y,
                                 unsigned char *Cb, unsigned char *Cr,
                                 int mirror_h, int mirror_v)
{
    int row_pixels = w;
    int row_pairs  = w / 2;
    for (int y = 0; y < h; y++) {
        int sy = mirror_v ? (h - 1 - y) : y;
        const unsigned char *srow = src + (size_t)sy * stride;
        unsigned char *yrow  = Y  + (size_t)y * row_pixels;
        for (int x = 0; x < w; x++) {
            int sx = mirror_h ? (w - 1 - x) : x;
            yrow[x] = srow[sx * 2 + 0];
        }
    }

    int chroma_rows = (h + 1) / 2;
    for (int cy = 0; cy < chroma_rows; cy++) {
        int y0 = cy * 2;
        int y1 = y0 + 1 < h ? y0 + 1 : y0;
        int sy0 = mirror_v ? (h - 1 - y0) : y0;
        int sy1 = mirror_v ? (h - 1 - y1) : y1;
        const unsigned char *srow0 = src + (size_t)sy0 * stride;
        const unsigned char *srow1 = src + (size_t)sy1 * stride;
        unsigned char *cbrow = Cb + (size_t)cy * row_pairs;
        unsigned char *crrow = Cr + (size_t)cy * row_pairs;
        for (int p = 0; p < row_pairs; p++) {
            int pair = mirror_h ? (row_pairs - 1 - p) : p;
            unsigned int cb = (unsigned int)srow0[pair * 4 + 1] +
                              (unsigned int)srow1[pair * 4 + 1];
            unsigned int cr = (unsigned int)srow0[pair * 4 + 3] +
                              (unsigned int)srow1[pair * 4 + 3];
            cbrow[p] = (unsigned char)((cb + 1U) / 2U);
            crrow[p] = (unsigned char)((cr + 1U) / 2U);
        }
    }
}

/* 单次 JPEG 编码后广播到直播最新帧槽和录像有界队列，慢录像盘只丢帧不反压。 */
static void *encode_thread(void *arg)
{
    ipcam_encode_ctx_t *ctx = arg;
    ipcam_frame_t frame;
    unsigned long encoded = 0, skipped = 0;
    unsigned long live_overwrites = 0;
    unsigned long record_drops = 0;
    struct timeval t0, t1;

    tjhandle tj = tjInitCompress();
    if (!tj) {
        MLOGE("tjInitCompress: %s\n", tjGetErrorStr());
        return NULL;
    }

    /* 配置可能来自已持久化的 /etc/ipcam.conf，启动日志必须打印实际编码质量，
     * 不能只打印编译期默认值，否则现场会误以为 q60 已经生效。 */
    MLOGI("encode thread start, quality=%u w=%d h=%d\n",
          ipcam_param_get_jpeg_quality(), ctx->width, ctx->height);
    gettimeofday(&t0, NULL);

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

    unsigned char *jpeg_buf = NULL;
    unsigned long  jpeg_size = 0;

    while (*ctx->running && ctx->service_running) {
        /* 编码速度低于采集速度时主动丢弃旧帧，避免延迟随时间线性增长。 */
        if (ipcam_ring_get_latest(ctx->in_rb, &frame) != 0) break;

        size_t stride = frame.stride ? frame.stride : (size_t)W * 2;
        if (stride < (size_t)W * 2 || frame.size < stride * (size_t)H) {
            MLOGW("frame size %zu/stride %zu invalid for %dx%d, skip\n",
                  frame.size, stride, W, H);
            skipped++;
            encode_add_stats(ctx, 0, 1);
            ipcam_ring_release(ctx->in_rb);
            continue;
        }

        unpack_yuyv_to_planar((const unsigned char *)frame.rawData, W, H, stride,
                              Yp, Cbp, Crp,
                              ipcam_param_get_mirror_horizontal(),
                              ipcam_param_get_mirror_vertical());

        const unsigned char *planes[3] = { Yp, Cbp, Crp };
        int strides[3] = { W, Wp, Wp };

        /*
         * libjpeg-turbo 2.1.x 参数顺序为 (handle, planes, width, strides, height, …)。
         * 曾误写成 strides/W 对调，会导致压缩失败、崩溃或垃圾 JPEG。
         */
        if (tjCompressFromYUVPlanes(tj, planes, W, strides, H, TJSAMP_420,
                                    &jpeg_buf, &jpeg_size,
                                    ipcam_param_get_jpeg_quality(),
                                    TJFLAG_FASTDCT) != 0) {
            MLOGW("tjCompressFromYUVPlanes: %s\n", tjGetErrorStr2(tj));
            skipped++;
            encode_add_stats(ctx, 0, 1);
            ipcam_ring_release(ctx->in_rb);
            continue;
        }

        /* 单帧 JPEG 超过 ring 槽容量时跳过本帧，避免把编码线程整条退出 */
        if (jpeg_size > ctx->out_rb->slot_bytes) {
            MLOGW("jpeg %lu > slot %zu, skip\n", jpeg_size, ctx->out_rb->slot_bytes);
            skipped++;
            encode_add_stats(ctx, 0, 1);
            ipcam_ring_release(ctx->in_rb);
            continue;
        }
        /* 压缩成功后即计入编码帧率；后续直播/录像队列各自丢帧单独统计。 */
        encode_add_stats(ctx, 1, 0);
        ipcam_frame_meta_t out_meta;
        memset(&out_meta, 0, sizeof(out_meta));
        out_meta.monotonic_ns = frame.monotonic_ns;
        out_meta.width = (uint16_t)W;
        out_meta.height = (uint16_t)H;
        out_meta.config_generation = frame.config_generation;
        int live_rc = ipcam_ring_try_append_latest_meta(ctx->out_rb, jpeg_buf,
                                                        jpeg_size, &out_meta);
        if (live_rc > 0) {
            /* 覆盖旧直播帧是低延迟策略的正常结果，不应算作编码失败。 */
            live_overwrites++;
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

        ipcam_ring_release(ctx->in_rb);
        /*
         * BCF2 的视频线程不会只在退出时给结果，而是打印首批帧和周期帧。
         * 这里记录输入序号、JPEG 大小、时间戳以及两个消费者的结果，能直接
         * 判断是编码慢、直播覆盖还是录像队列溢出，同时限制串口输出频率。
         */
        if (encoded <= 30 || (encoded % 30) == 0) {
            MLOGI("frame no=%lu src_seq=%lu ts=%llu jpeg=%lu live_rc=%d record_rc=%d\n",
                  encoded, frame.seqNo, (unsigned long long)frame.monotonic_ns,
                  jpeg_size, live_rc, record_rc);
        }
    }

    free(Yp); free(Cbp); free(Crp);
    if (jpeg_buf) tjFree(jpeg_buf);
    tjDestroy(tj);

    gettimeofday(&t1, NULL);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    MLOGI("encode thread exit, encoded=%lu skipped=%lu live_overwrites=%lu record_drops=%lu avg_fps=%.1f\n",
          encoded, skipped, live_overwrites, record_drops,
          sec > 0 ? encoded / sec : 0);
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
    memset(ctx, 0, sizeof(*ctx));
    ctx->in_rb = in;
    ctx->out_rb = out;
    ctx->aux_rb = aux;
    ctx->running = running;
    ctx->service_running = 1;
    pthread_mutex_init(&ctx->stats_mtx, NULL);
    ctx->quality = IPCAM_JPEG_QUALITY;
    ctx->width  = src_w > 0 ? src_w : IPCAM_CAPTURE_WIDTH;
    ctx->height = src_h > 0 ? src_h : IPCAM_CAPTURE_HEIGHT;

    if (pthread_create(&ctx->thread, NULL, encode_thread, ctx) != 0) {
        MLOGE("pthread_create encode failed\n");
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    MLOGI("encode service ready: input=%dx%d quality=%u live_rb=%s record_rb=%s\n",
          ctx->width, ctx->height, ipcam_param_get_jpeg_quality(),
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
