#define _GNU_SOURCE
#include "ipcam_display.h"
#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "ipcam_config.h"

/* YCbCr -> RGB ITU-R BT.601 近似，输出 RGB565（5:6:5） */
static inline unsigned short yuyv_to_rgb565(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    if (c < 0) c = 0;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (unsigned short)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/*
 * 把 src（YUYV w×h）按最临近插值缩放到 dst（RGB565 dw×dh）。
 * 用 finfo.line_length 作 stride；fb_bpp 必须 = 16。
 * src_w 必须 >= 2（YUYV 是 4:2:2 packed，每两像素一个 Cb/Cr）。
 */
static void yuyv_to_rgb565_scaled(const unsigned char *src, int sw, int sh,
                                  unsigned short *dst, int dw, int dh,
                                  int dst_stride_pixels)
{
    if (sw < 2) return;  /* YUYV 4:2:2 需要至少 2 像素宽 */
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        const unsigned char *src_row = src + (size_t)sy * sw * 2;
        unsigned short *dst_row = dst + (size_t)dy * dst_stride_pixels;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            /* YUYV 每两像素一对 (Cb, Cr)；sx 必须偶数对齐 */
            int sx0 = sx & ~1;
            /* clamp 到 sw-2，避免 sx==sw-1 时 +2/+3 越界到下一行 */
            if (sx0 >= sw - 1) sx0 = sw - 2;
            if (sx0 < 0) sx0 = 0;
            int y0 = src_row[sx0 * 2 + 0];
            int u  = src_row[sx0 * 2 + 1];
            int y1 = src_row[sx0 * 2 + 2];
            int v  = src_row[sx0 * 2 + 3];
            int yy = (sx & 1) ? y1 : y0;
            dst_row[dx] = yuyv_to_rgb565(yy, u, v);
        }
    }
}

static int display_open_fb(ipcam_display_ctx_t *ctx)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    ctx->fb_fd = open(IPCAM_FB_DEV, O_RDWR);
    if (ctx->fb_fd < 0) {
        MLOGE("open %s: %s\n", IPCAM_FB_DEV, strerror(errno));
        return -1;
    }
    if (ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        MLOGE("FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        return -1;
    }
    if (ioctl(ctx->fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        MLOGE("FBIOGET_FSCREENINFO: %s\n", strerror(errno));
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        return -1;
    }

    ctx->fb_w  = vinfo.xres;
    ctx->fb_h  = vinfo.yres;
    ctx->fb_bpp = vinfo.bits_per_pixel;
    ctx->fb_line_length = (int)finfo.line_length;
    ctx->fb_size = finfo.smem_len;

    if (ctx->fb_bpp != 16) {
        MLOGE("fb bpp=%d not supported (only 16)\n", ctx->fb_bpp);
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        return -1;
    }
    /* line_length 通常 = xres*2；如果硬件有 padding，需 ≥ xres*2 */
    if (ctx->fb_line_length < ctx->fb_w * 2) {
        MLOGE("fb line_length=%d too small for w=%d bpp=16\n",
              ctx->fb_line_length, ctx->fb_w);
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        return -1;
    }

    ctx->fb_base = mmap(NULL, ctx->fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fb_fd, 0);
    if (ctx->fb_base == MAP_FAILED) {
        MLOGE("mmap fb: %s\n", strerror(errno));
        ctx->fb_base = NULL;
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        return -1;
    }

    ctx->out_w = ctx->fb_w;
    ctx->out_h = ctx->fb_h;

    /* 整屏刷黑 */
    memset(ctx->fb_base, 0, ctx->fb_size);
    MLOGI("fb ready: %dx%d bpp=%d line_length=%d size=%zu\n",
          ctx->fb_w, ctx->fb_h, ctx->fb_bpp, ctx->fb_line_length, ctx->fb_size);
    return 0;
}

static void *display_thread(void *arg)
{
    ipcam_display_ctx_t *ctx = arg;
    ipcam_frame_t frame;
    unsigned long frames = 0;
    struct timeval t0, t1;
    int dst_stride_pixels = ctx->fb_line_length / 2;  /* 16bpp = 2 bytes/pixel */

    MLOGI("display thread start, out=%dx%d\n", ctx->out_w, ctx->out_h);
    gettimeofday(&t0, NULL);

    while (*ctx->running) {
        if (ipcam_ring_get(ctx->rb, &frame) != 0) break;

        /*
         * src 宽高由 capture 协商结果传入（ctx->src_w / src_h）。
         * frame.size 应 == src_w * src_h * 2；不一致则丢弃并警告。
         */
        int src_w = ctx->src_w;
        int src_h = ctx->src_h;
        size_t expected = (size_t)src_w * src_h * 2;
        if (frame.size != expected) {
            MLOGW("frame size %zu != %d*%d*2 (%zu), skip\n",
                  frame.size, src_w, src_h, expected);
            ipcam_ring_release(ctx->rb);
            continue;
        }

        yuyv_to_rgb565_scaled(frame.rawData, src_w, src_h,
                              ctx->fb_base, ctx->out_w, ctx->out_h, dst_stride_pixels);
        frames++;

        ipcam_ring_release(ctx->rb);
    }

    gettimeofday(&t1, NULL);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    MLOGI("display thread exit, frames=%lu avg_fps=%.1f\n",
          frames, sec > 0 ? frames / sec : 0);
    return NULL;
}

int ipcam_display_start(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                        int src_w, int src_h,
                        volatile sig_atomic_t *running)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->fb_fd = -1;
    ctx->fb_base = NULL;
    ctx->rb = rb;
    ctx->running = running;
    ctx->src_w = src_w > 0 ? src_w : IPCAM_CAPTURE_WIDTH;
    ctx->src_h = src_h > 0 ? src_h : IPCAM_CAPTURE_HEIGHT;

    if (display_open_fb(ctx) < 0) return -1;

    if (pthread_create(&ctx->thread, NULL, display_thread, ctx) != 0) {
        MLOGE("pthread_create display failed\n");
        if (ctx->fb_base) munmap(ctx->fb_base, ctx->fb_size);
        if (ctx->fb_fd >= 0) close(ctx->fb_fd);
        return -1;
    }
    return 0;
}

void ipcam_display_stop(ipcam_display_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->running) *ctx->running = 0;

    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }

    if (ctx->fb_base) {
        munmap(ctx->fb_base, ctx->fb_size);
        ctx->fb_base = NULL;
    }
    if (ctx->fb_fd >= 0) {
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
    }
}