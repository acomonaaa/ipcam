#define _GNU_SOURCE
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

static void *encode_thread(void *arg)
{
    ipcam_encode_ctx_t *ctx = arg;
    ipcam_frame_t frame;
    unsigned long encoded = 0, skipped = 0;
    struct timeval t0, t1;

    tjhandle tj = tjInitCompress();
    if (!tj) {
        MLOGE("tjInitCompress: %s\n", tjGetErrorStr());
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

        const unsigned char *planes[3] = { Yp, Cbp, Crp };
        int strides[3] = { W, Wp, Wp };

        /*
         * libjpeg-turbo 2.1.x 参数顺序为 (handle, planes, width, strides, height, …)。
         * 曾误写成 strides/W 对调，会导致压缩失败、崩溃或垃圾 JPEG。
         */
        if (tjCompressFromYUVPlanes(tj, planes, W, strides, H, TJSAMP_422,
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
}