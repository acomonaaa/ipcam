#define _GNU_SOURCE
#include "ipcam_encode.h"
#include "ipcam_log.h"
#include "ipcam_param.h"
#include "ipcam_scale.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <turbojpeg.h>
#include <unistd.h>

#include "ipcam_config.h"

/*
 * YUYV (packed 4:2:2) -> planar 4:2:2 (Y 整分辨率 + Cb/Cr 半宽整高)
 *
 * YUYV 字节布局：
 *   b0=Y0  b1=Cb0  b2=Y1  b3=Cr0  b4=Y2  b5=Cb1  b6=Y3  b7=Cr1  ...
 *
 * 输出：
 *   Y[i]   (i in [0, W*H))              = src[(i/W)*W*2 + (i%W)*2 + 0]
 *   Cb[j]  (j in [0, (W/2)*H))          = src[(j/(W/2))*W*2 + (j%(W/2))*4 + 1]
 *   Cr[j]  (j in [0, (W/2)*H))          = src[(j/(W/2))*W*2 + (j%(W/2))*4 + 3]
 *
 * 为什么拆分放在编码线程而不是采集线程：ring 里存 packed 原始帧，
 * display（packed 转 RGB565）与 encode（packed 转平面 YUV）各取所需；
 * 拆分每帧只做一次、无重复劳动，而采集线程保持纯拷贝，避免生产者变重
 * 导致两路消费同时丢帧。
 */
static void unpack_yuyv_to_planar(const unsigned char *src, int w, int h,
                                 unsigned char *Y, unsigned char *Cb, unsigned char *Cr)
{
    int row_pixels = w;
    int row_pairs  = w / 2;
    int row_bytes  = w * 2;

    for (int y = 0; y < h; y++) {
        const unsigned char *srow = src + (size_t)y * row_bytes;
        unsigned char *yrow  = Y  + (size_t)y * row_pixels;
        unsigned char *cbrow = Cb + (size_t)y * row_pairs;
        unsigned char *crrow = Cr + (size_t)y * row_pairs;
        for (int x = 0; x < w; x++) {
            yrow[x] = srow[x * 2 + 0];
        }
        for (int p = 0; p < row_pairs; p++) {
            cbrow[p] = srow[p * 4 + 1];
            crrow[p] = srow[p * 4 + 3];
        }
    }
}

/*
 * 统一检查平面分配的乘法，避免异常配置在 malloc 前发生 size_t 溢出。
 * 这里不限制业务分辨率，只负责验证“宽×高”能否安全表示；具体设备上限
 * 由 V4L2 协商和缩放模块的定点计算上限共同约束。
 */
static int checked_plane_bytes(int width, int height, size_t *bytes)
{
    if (width <= 0 || height <= 0 || !bytes) return -1;
    if ((size_t)width > SIZE_MAX / (size_t)height) return -1;
    *bytes = (size_t)width * (size_t)height;
    return 0;
}

/*
 * 色度平面最临近缩放：4:2:2 色度本身是低频半分辨率信号，
 * 双线性的收益/代价比不高，最临近足够且最便宜。
 */
static void plane_nearest_scale(const unsigned char *src, int sw, int sh,
                                unsigned char *dst, int dw, int dh)
{
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        if (sy >= sh) sy = sh - 1;
        const unsigned char *srow = src + (size_t)sy * sw;
        unsigned char *drow = dst + (size_t)dy * dw;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            if (sx >= sw) sx = sw - 1;
            drow[dx] = srow[sx];
        }
    }
}

static void *encode_thread(void *arg)
{
    ipcam_encode_ctx_t *ctx = arg;
    ipcam_frame_t frame;
    unsigned long encoded = 0, skipped = 0;
    struct timeval t0, t1;

    tjhandle tj = tjInitCompress();
    if (!tj) {
        MLOGE("tjInitCompress: %s\n", tjGetErrorStr());
        /* 编码线程无法启动时，不能让主循环继续报告服务健康。 */
        if (ctx->running) *ctx->running = 0;
        return NULL;
    }

    MLOGI("encode thread start, quality=%d w=%d h=%d\n",
          ctx->quality, ctx->width, ctx->height);
    gettimeofday(&t0, NULL);

    int W = ctx->width;
    int H = ctx->height;
    int Wp = W / 2;       /* pair width */

    /* planar 工作缓冲：每次循环复用，避免每帧 malloc */
    unsigned char *Yp  = malloc((size_t)W * H);
    unsigned char *Cbp = malloc((size_t)Wp * H);
    unsigned char *Crp = malloc((size_t)Wp * H);
    if (!Yp || !Cbp || !Crp) {
        MLOGE("alloc YUV planes failed\n");
        free(Yp); free(Cbp); free(Crp);
        tjDestroy(tj);
        if (ctx->running) *ctx->running = 0;
        return NULL;
    }

    unsigned char *jpeg_buf = NULL;
    unsigned long  jpeg_size = 0;

    while (*ctx->running) {
        if (ipcam_ring_get(ctx->in_rb, &frame) != 0) break;

        size_t expected = (size_t)W * H * 2;
        if (frame.size != expected) {
            MLOGW("frame size %zu != %d*%d*2, skip\n", frame.size, W, H);
            skipped++;
            ipcam_ring_release(ctx->in_rb);
            continue;
        }

        unpack_yuyv_to_planar((const unsigned char *)frame.rawData, W, H, Yp, Cbp, Crp);

        /*
         * 输出分辨率 != 采集分辨率时，在 planar 域逐平面缩放后再编码：
         * Y 双线性（保细节平滑）、色度最临近（低频信号足够）。
         * 选 planar 域而非 packed 域缩放：packed 4:2:2 的色度有偶像素
         * 对齐约束（见 display 的 sx&~1），planar 各平面独立缩放更干净。
         */
        const unsigned char *planes[3];
        int strides[3];
        int enc_w, enc_h;
        if (ctx->scale_on) {
            if (ipcam_scale_plane_bilinear(Yp, W, H, ctx->Ys,
                                            ctx->out_w, ctx->out_h,
                                            ctx->xs0, ctx->xfrac) != 0) {
                MLOGE("invalid Y-plane scale parameters, stopping encoder\n");
                if (ctx->running) *ctx->running = 0;
                ipcam_ring_release(ctx->in_rb);
                break;
            }
            plane_nearest_scale(Cbp, Wp, H, ctx->Cbs, ctx->out_w / 2, ctx->out_h);
            plane_nearest_scale(Crp, Wp, H, ctx->Crs, ctx->out_w / 2, ctx->out_h);
            planes[0] = ctx->Ys; planes[1] = ctx->Cbs; planes[2] = ctx->Crs;
            enc_w = ctx->out_w;
        } else {
            planes[0] = Yp; planes[1] = Cbp; planes[2] = Crp;
            enc_w = W;
        }
        enc_h = ctx->out_h;
        strides[0] = enc_w; strides[1] = enc_w / 2; strides[2] = enc_w / 2;

        /*
         * libjpeg-turbo 2.1.x 参数顺序为 (handle, planes, width, strides, height, …)。
         * 曾误写成 strides/W 对调，会导致压缩失败、崩溃或垃圾 JPEG。
         */
        if (tjCompressFromYUVPlanes(tj, planes, enc_w, strides, enc_h, TJSAMP_422,
                                    &jpeg_buf, &jpeg_size,
                                    ipcam_param_get_jpeg_quality(),
                                    TJFLAG_FASTDCT) != 0) {
            MLOGW("tjCompressFromYUVPlanes: %s\n", tjGetErrorStr2(tj));
            skipped++;
            ipcam_ring_release(ctx->in_rb);
            continue;
        }

        /* 单帧 JPEG 超过 ring 槽容量时跳过本帧，避免把编码线程整条退出 */
        if (jpeg_size > ctx->out_rb->slot_bytes) {
            MLOGW("jpeg %lu > slot %zu, skip\n", jpeg_size, ctx->out_rb->slot_bytes);
            skipped++;
            ipcam_ring_release(ctx->in_rb);
            continue;
        }
        if (ipcam_ring_append(ctx->out_rb, jpeg_buf, jpeg_size) == 0) {
            encoded++;
        } else {
            /* 阻塞式 append 仅在 ring 已关闭时失败（满会等待）；关闭则结束编码线程 */
            ipcam_ring_release(ctx->in_rb);
            break;
        }

        ipcam_ring_release(ctx->in_rb);
    }

    free(Yp); free(Cbp); free(Crp);
    if (jpeg_buf) tjFree(jpeg_buf);
    tjDestroy(tj);

    gettimeofday(&t1, NULL);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    MLOGI("encode thread exit, encoded=%lu skipped=%lu avg_fps=%.1f\n",
          encoded, skipped, sec > 0 ? encoded / sec : 0);
    return NULL;
}

int ipcam_encode_start(ipcam_encode_ctx_t *ctx,
                       ipcam_ring_buffer_t *in,
                       ipcam_ring_buffer_t *out,
                       int src_w, int src_h,
                       volatile sig_atomic_t *running)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->in_rb = in;
    ctx->out_rb = out;
    ctx->running = running;
    ctx->quality = IPCAM_JPEG_QUALITY;
    ctx->width  = src_w > 0 ? src_w : IPCAM_CAPTURE_WIDTH;
    ctx->height = src_h > 0 ? src_h : IPCAM_CAPTURE_HEIGHT;

    if (pthread_create(&ctx->thread, NULL, encode_thread, ctx) != 0) {
        MLOGE("pthread_create encode failed\n");
        return -1;
    }
    return 0;
}

void ipcam_encode_stop(ipcam_encode_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->running) *ctx->running = 0;

    /* ring 关闭由 main.c cleanup_all 统一做（避免双重 close） */

    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }

    /* 释放缩放路径的常驻缓冲（scale_on=0 时均为 NULL，free 空指针安全） */
    free(ctx->Ys); free(ctx->Cbs); free(ctx->Crs);
    free(ctx->xs0); free(ctx->xfrac);
    ctx->Ys = ctx->Cbs = ctx->Crs = NULL;
    ctx->xs0 = ctx->xfrac = NULL;
}
