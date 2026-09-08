#define _GNU_SOURCE
#include "ipcam_display.h"
#include "ipcam_frame_diag.h"
#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ipcam_config.h"

#define IPCAM_DISPLAY_LOG_MODULE "DISP"
#define IPCAM_DISPLAY_PAN_WARN_US 100000ULL

static uint64_t display_monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static void display_log_fb_layout(const struct fb_var_screeninfo *vinfo,
                                  const struct fb_fix_screeninfo *finfo,
                                  const char *prefix)
{
    if (!vinfo || !finfo) return;
    MLOGI_M(IPCAM_DISPLAY_LOG_MODULE,
            "%s fb var={xres=%u yres=%u xv=%u yv=%u xoff=%u yoff=%u "
            "bpp=%u activate=0x%x red=%u/%u green=%u/%u blue=%u/%u "
            "transp=%u/%u} fix={id=%s smem_start=%lx smem_len=%u "
            "line_length=%u ypanstep=%u ywrapstep=%u}\n",
            prefix ? prefix : "fb layout:",
            vinfo->xres, vinfo->yres, vinfo->xres_virtual,
            vinfo->yres_virtual, vinfo->xoffset, vinfo->yoffset,
            vinfo->bits_per_pixel, vinfo->activate,
            vinfo->red.offset, vinfo->red.length,
            vinfo->green.offset, vinfo->green.length,
            vinfo->blue.offset, vinfo->blue.length,
            vinfo->transp.offset, vinfo->transp.length,
            finfo->id, (unsigned long)finfo->smem_start, finfo->smem_len,
            finfo->line_length, finfo->ypanstep, finfo->ywrapstep);
}

/* YCbCr -> RGB ITU-R BT.601 近似，输出 RGB565（5:6:5） */
static inline unsigned short yuv_to_rgb565(int y, int u, int v)
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
 * 把 src（packed 4:2:2，字节序由 IPCAM_CAP_PIXFMT 决定）按最临近插值
 * 缩放到 dst（RGB565 dw×dh）。
 * 用 finfo.line_length 作 stride；fb_bpp 必须 = 16。
 * src_w 必须 >= 2（4:2:2 packed 每两像素共享一组 Cb/Cr）。
 */
static void yuv422_packed_to_rgb565_scaled(const unsigned char *src, int sw, int sh,
                                           size_t src_stride_bytes,
                                           unsigned short *dst, int dw, int dh,
                                           int dst_stride_pixels)
{
    if (sw < 2 || src_stride_bytes < (size_t)sw * 2U) return;
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        const unsigned char *src_row = src + (size_t)sy * src_stride_bytes;
        unsigned short *dst_row = dst + (size_t)dy * dst_stride_pixels;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            /* packed 4:2:2 每两像素一对 (Cb, Cr)；sx 必须偶数对齐 */
            int sx0 = sx & ~1;
            /* clamp 到 sw-2，避免 sx==sw-1 时 +2/+3 越界到下一行 */
            if (sx0 >= sw - 1) sx0 = sw - 2;
            if (sx0 < 0) sx0 = 0;
#if IPCAM_CAP_PIXFMT == 1
            /* UYVY 字节序：Cb Y0 Cr Y1 */
            int u  = src_row[sx0 * 2 + 0];
            int y0 = src_row[sx0 * 2 + 1];
            int v  = src_row[sx0 * 2 + 2];
            int y1 = src_row[sx0 * 2 + 3];
#else
            /* YUYV 字节序：Y0 Cb Y1 Cr */
            int y0 = src_row[sx0 * 2 + 0];
            int u  = src_row[sx0 * 2 + 1];
            int y1 = src_row[sx0 * 2 + 2];
            int v  = src_row[sx0 * 2 + 3];
#endif
            int yy = (sx & 1) ? y1 : y0;
            dst_row[dx] = yuv_to_rgb565(yy, u, v);
        }
    }
}

static int display_get_fb_info(int fd, struct fb_var_screeninfo *vinfo,
                               struct fb_fix_screeninfo *finfo)
{
    if (ioctl(fd, FBIOGET_VSCREENINFO, vinfo) < 0) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        return -1;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, finfo) < 0) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "FBIOGET_FSCREENINFO: %s\n", strerror(errno));
        return -1;
    }
    finfo->id[sizeof(finfo->id) - 1] = '\0';
    return 0;
}

static int display_fb_layout_equal(const ipcam_display_ctx_t *ctx,
                                   const struct fb_var_screeninfo *vinfo,
                                   const struct fb_fix_screeninfo *finfo)
{
    const struct fb_var_screeninfo *old_var;
    const struct fb_fix_screeninfo *old_fix;

    if (!ctx || !vinfo || !finfo) return 0;
    old_var = &ctx->fb_var;
    old_fix = &ctx->fb_fix;

    /* yoffset/xoffset 是翻页状态，不属于“布局变化”，单独校验。 */
    return old_var->xres == vinfo->xres && old_var->yres == vinfo->yres &&
           old_var->xres_virtual == vinfo->xres_virtual &&
           old_var->yres_virtual == vinfo->yres_virtual &&
           old_var->bits_per_pixel == vinfo->bits_per_pixel &&
           old_var->red.offset == vinfo->red.offset &&
           old_var->red.length == vinfo->red.length &&
           old_var->green.offset == vinfo->green.offset &&
           old_var->green.length == vinfo->green.length &&
           old_var->blue.offset == vinfo->blue.offset &&
           old_var->blue.length == vinfo->blue.length &&
           old_var->transp.offset == vinfo->transp.offset &&
           old_var->transp.length == vinfo->transp.length &&
           old_fix->smem_start == finfo->smem_start &&
           old_fix->smem_len == finfo->smem_len &&
           old_fix->line_length == finfo->line_length &&
           old_fix->ypanstep == finfo->ypanstep &&
           old_fix->ywrapstep == finfo->ywrapstep;
}

static unsigned short *display_page(ipcam_display_ctx_t *ctx, int page);

static int display_probe_page(const ipcam_display_ctx_t *ctx, int page,
                              ipcam_frame_probe_t *probe)
{
    unsigned short *ptr;

    if (!ctx || !probe || page < 0 || page >= ctx->fb_page_count)
        return -1;
    ptr = display_page((ipcam_display_ctx_t *)ctx, page);
    if (!ptr) return -1;
    return ipcam_frame_probe_pixels(ptr, ctx->fb_page_bytes,
                                    ctx->fb_w, ctx->fb_h, 2,
                                    (size_t)ctx->fb_line_length, probe);
}

static int display_probe_equal(const ipcam_frame_probe_t *a,
                               const ipcam_frame_probe_t *b)
{
    if (!a || !b || a->global != b->global) return 0;
    for (int i = 0; i < IPCAM_FRAME_PROBE_QUADRANTS; i++)
        if (a->quadrant[i] != b->quadrant[i]) return 0;
    return 1;
}

static void display_log_probe(const char *label, const ipcam_frame_probe_t *probe)
{
    if (!probe) return;
    MLOGD_M(IPCAM_DISPLAY_LOG_MODULE,
            "%s probe=%08x q=%08x/%08x/%08x/%08x\n",
            label ? label : "probe:", probe->global,
            probe->quadrant[0], probe->quadrant[1],
            probe->quadrant[2], probe->quadrant[3]);
}

/*
 * 周期性重新读取 framebuffer。mxsfb 的页地址依赖 line_length/yoffset；任何
 * 外部 PUT_VSCREENINFO 或 console 改写布局都可能把下一帧写到错误位置，因此
 * 发现布局变化后立即停止显示线程，而不是继续制造更难复现的左右错位。
 */
static int display_verify_fb_runtime(ipcam_display_ctx_t *ctx)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    unsigned int expected_yoffset;

    if (!ctx) return -1;
    if (display_get_fb_info(ctx->fb_fd, &vinfo, &finfo) < 0) {
        ctx->fb_query_errors++;
        return -1;
    }
    if (!display_fb_layout_equal(ctx, &vinfo, &finfo)) {
        ctx->fb_layout_changes++;
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "framebuffer layout changed at runtime (count=%lu)\n",
                ctx->fb_layout_changes);
        display_log_fb_layout(&vinfo, &finfo, "new fb layout:");
        return -1;
    }

    expected_yoffset = ctx->fb_flip_enabled
        ? (unsigned int)(ctx->fb_current_page * ctx->fb_h) : 0U;
    if (vinfo.xoffset != 0 || vinfo.yoffset != expected_yoffset) {
        ctx->unexpected_yoffset++;
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "unexpected framebuffer offset: got=%u/%u expected=0/%u "
                "page=%d count=%lu\n",
                vinfo.xoffset, vinfo.yoffset, expected_yoffset,
                ctx->fb_current_page, ctx->unexpected_yoffset);
        return -1;
    }
    return 0;
}

/*
 * 尝试把 framebuffer 扩成上下两页。mxsfb 的 panning 单位是像素，且驱动在
 * FBIOPAN_DISPLAY 内等待下一次帧完成；因此 page_bytes 必须按 line_length
 * 计算，不能只按 xres*yres 估算，否则存在行 padding 时会产生错位。
 * 返回 1 表示可用双页，返回 0 表示应退化到临时缓冲路径。
 */
static int display_prepare_double_buffer(int fd,
                                         struct fb_var_screeninfo *vinfo,
                                         struct fb_fix_screeninfo *finfo)
{
    uint64_t page_bytes;
    uint64_t required_bytes;
    __u32 required_yres_virtual;

    if (!vinfo || !finfo || vinfo->yres == 0 ||
        vinfo->yres > UINT_MAX / 2U) {
        MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                "fb yres=%u cannot provide two pages; using staging buffer\n",
                vinfo ? vinfo->yres : 0U);
        return 0;
    }

    required_yres_virtual = vinfo->yres * 2U;
    page_bytes = (uint64_t)finfo->line_length * vinfo->yres;
    required_bytes = page_bytes * 2U;
    if (finfo->line_length == 0 || page_bytes == 0 ||
        required_bytes > finfo->smem_len) {
        MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                "fb memory too small for two pages: line_length=%u yres=%u "
                "smem_len=%u required=%llu; using staging buffer\n",
                finfo->line_length, vinfo->yres, finfo->smem_len,
                (unsigned long long)required_bytes);
        return 0;
    }

    if (vinfo->yres_virtual < required_yres_virtual) {
        struct fb_var_screeninfo requested = *vinfo;
        requested.yres_virtual = required_yres_virtual;
        requested.xoffset = 0;
        requested.yoffset = 0;
        requested.activate = FB_ACTIVATE_NOW;

        if (ioctl(fd, FBIOPUT_VSCREENINFO, &requested) < 0) {
            MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                    "FBIOPUT_VSCREENINFO yres_virtual=%u failed: %s; "
                    "using staging buffer\n",
                    required_yres_virtual, strerror(errno));
            return 0;
        }
        if (display_get_fb_info(fd, vinfo, finfo) < 0) {
            MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                    "cannot re-read framebuffer after enabling two pages; "
                    "using staging buffer\n");
            return 0;
        }
        if (vinfo->yres == 0 || vinfo->yres > UINT_MAX / 2U) {
            MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                    "framebuffer changed to invalid yres=%u; using staging buffer\n",
                    vinfo->yres);
            return 0;
        }
        /* PUT 后驱动可能修正可视高度，双页条件必须按修正后的高度重算。 */
        required_yres_virtual = vinfo->yres * 2U;
    }

    page_bytes = (uint64_t)finfo->line_length * vinfo->yres;
    required_bytes = page_bytes * 2U;
    if (vinfo->yres_virtual < required_yres_virtual ||
        finfo->ypanstep == 0 || page_bytes == 0 ||
        required_bytes > finfo->smem_len) {
        MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                "fb cannot pan two complete pages: virtual_y=%u ypanstep=%u "
                "line_length=%u smem_len=%u required=%llu; using staging buffer\n",
                vinfo->yres_virtual, finfo->ypanstep, finfo->line_length,
                finfo->smem_len, (unsigned long long)required_bytes);
        return 0;
    }
    return 1;
}

static unsigned short *display_page(ipcam_display_ctx_t *ctx, int page)
{
    if (!ctx || !ctx->fb_base || page < 0 || page >= ctx->fb_page_count)
        return NULL;
    return (unsigned short *)((unsigned char *)ctx->fb_base +
                              (size_t)page * ctx->fb_page_bytes);
}

/*
 * 只让驱动切换“完整后台页”。mxsfb 的实现会把下一页地址写入 next_buf，
 * 并等待当前帧完成后才返回；应用因此不会在 LCD 正在扫描时修改前台页。
 */
static int display_pan_to_page(ipcam_display_ctx_t *ctx, int page)
{
    struct fb_var_screeninfo var;
    struct fb_var_screeninfo actual;
    uint64_t yoffset;
    uint64_t started_us;
    uint64_t finished_us;
    uint64_t elapsed_us;
    int saved_errno;

    if (!ctx || !ctx->fb_flip_enabled || page < 0 || page >= 2)
        return -1;
    yoffset = (uint64_t)page * ctx->fb_h;
    if (yoffset + ctx->fb_h > ctx->fb_yres_virtual || yoffset > UINT_MAX) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "invalid framebuffer page=%d yoffset=%llu virtual_y=%u\n",
                page, (unsigned long long)yoffset, ctx->fb_yres_virtual);
        return -1;
    }

    var = ctx->fb_var;
    var.xoffset = 0;
    var.yoffset = (unsigned int)yoffset;
    var.activate = FB_ACTIVATE_NOW;
    started_us = display_monotonic_us();
    if (ioctl(ctx->fb_fd, FBIOPAN_DISPLAY, &var) < 0) {
        saved_errno = errno;
        finished_us = display_monotonic_us();
        elapsed_us = finished_us >= started_us ? finished_us - started_us : 0;
        ctx->pan_errors++;
        if (saved_errno == ETIMEDOUT)
            ctx->pan_timeouts++;
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "FBIOPAN_DISPLAY page=%d yoffset=%u failed after %lluus: %s "
                "errors=%lu timeouts=%lu\n",
                page, var.yoffset, (unsigned long long)elapsed_us,
                strerror(saved_errno), ctx->pan_errors, ctx->pan_timeouts);
        return -1;
    }
    finished_us = display_monotonic_us();
    elapsed_us = finished_us >= started_us ? finished_us - started_us : 0;
    ctx->pan_calls++;
    ctx->pan_total_us += elapsed_us;
    if (ctx->pan_min_us == 0 || elapsed_us < ctx->pan_min_us)
        ctx->pan_min_us = elapsed_us;
    if (elapsed_us > ctx->pan_max_us)
        ctx->pan_max_us = elapsed_us;
    if (elapsed_us > IPCAM_DISPLAY_PAN_WARN_US) {
        ctx->pan_wait_over_threshold++;
        MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                "FBIOPAN_DISPLAY slow: page=%d yoffset=%u elapsed=%lluus "
                "(threshold=%lluus)\n",
                page, var.yoffset, (unsigned long long)elapsed_us,
                (unsigned long long)IPCAM_DISPLAY_PAN_WARN_US);
    }

    /*
     * ioctl 成功只说明驱动接受了请求；再读一次 yoffset 才能确认显示控制器
     * 实际采用了预期页。若不一致，继续写页会把错位扩大成持续性花屏。
     */
    if (ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &actual) < 0) {
        ctx->fb_query_errors++;
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "FBIOGET_VSCREENINFO after pan page=%d failed: %s\n",
                page, strerror(errno));
        return -1;
    }
    if (actual.xoffset != 0 || actual.yoffset != (unsigned int)yoffset) {
        ctx->unexpected_yoffset++;
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "FBIOPAN_DISPLAY accepted unexpected offset: page=%d "
                "requested=0/%u actual=%u/%u count=%lu\n",
                page, (unsigned int)yoffset, actual.xoffset, actual.yoffset,
                ctx->unexpected_yoffset);
        return -1;
    }

    /*
     * DEBUG 级别保留每次翻页的完整证据：目标页地址、请求/实际偏移和 ioctl
     * 耗时。默认 INFO 不会被 16.5 fps 的逐页日志淹没，现场可用
     * IPCAM_LOG_LEVEL=5 打开这一条证据链。
     */
    MLOGD_M(IPCAM_DISPLAY_LOG_MODULE,
            "pan: target_page=%d addr=%p requested_yoffset=%u "
            "actual_xoffset=%u actual_yoffset=%u elapsed_us=%llu "
            "current_page=%d\n",
            page, (void *)display_page(ctx, page), (unsigned int)yoffset,
            actual.xoffset, actual.yoffset, (unsigned long long)elapsed_us,
            page);

    ctx->fb_var = var;
    ctx->fb_var.xoffset = actual.xoffset;
    ctx->fb_var.yoffset = actual.yoffset;
    ctx->fb_current_page = page;
    return 0;
}

/* 单页 framebuffer 的最后一道保护：尽量等到 VSYNC 再做一次整页复制。 */
static int display_wait_for_vsync(ipcam_display_ctx_t *ctx)
{
    uint32_t vsync = 0;

    if (!ctx || ctx->fb_vsync_available == 0)
        return -1;
    if (ioctl(ctx->fb_fd, FBIO_WAITFORVSYNC, &vsync) == 0) {
        ctx->fb_vsync_available = 1;
        return 0;
    }

    ctx->fb_vsync_available = 0;
    MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
            "FBIO_WAITFORVSYNC unavailable: %s; single-buffer copy may tear\n",
            strerror(errno));
    return -1;
}

static void display_release_fb(ipcam_display_ctx_t *ctx)
{
    if (!ctx) return;
    free(ctx->fb_staging);
    ctx->fb_staging = NULL;
    ctx->fb_staging_size = 0;
    if (ctx->fb_base) {
        munmap(ctx->fb_base, ctx->fb_size);
        ctx->fb_base = NULL;
    }
    if (ctx->fb_fd >= 0) {
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
    }
}

static int display_open_fb(ipcam_display_ctx_t *ctx)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    uint64_t page_bytes;
    int double_buffer;

    ctx->fb_fd = open(IPCAM_FB_DEV, O_RDWR);
    if (ctx->fb_fd < 0) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE, "open %s: %s\n", IPCAM_FB_DEV, strerror(errno));
        return -1;
    }
    if (display_get_fb_info(ctx->fb_fd, &vinfo, &finfo) < 0)
        goto fail;
    display_log_fb_layout(&vinfo, &finfo, "initial fb layout:");

    if (vinfo.xres == 0 || vinfo.yres == 0 || vinfo.xres > INT_MAX ||
        vinfo.yres > INT_MAX || vinfo.bits_per_pixel != 16) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "unsupported fb geometry: %ux%u bpp=%u (need RGB565)\n",
                vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
        goto fail;
    }
    if (finfo.line_length > INT_MAX ||
        (uint64_t)finfo.line_length < (uint64_t)vinfo.xres * 2U) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "invalid fb line_length=%u for w=%u bpp=16\n",
                finfo.line_length, vinfo.xres);
        goto fail;
    }
    page_bytes = (uint64_t)finfo.line_length * vinfo.yres;
    if (page_bytes == 0 || page_bytes > SIZE_MAX || finfo.smem_len < page_bytes) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "invalid fb memory: line_length=%u yres=%u smem_len=%u\n",
                finfo.line_length, vinfo.yres, finfo.smem_len);
        goto fail;
    }

    /* 先申请双页；驱动拒绝时保留单页并走 staging + VSYNC 退化路径。 */
    double_buffer = display_prepare_double_buffer(ctx->fb_fd, &vinfo, &finfo);
    page_bytes = (uint64_t)finfo.line_length * vinfo.yres;
    if (vinfo.xres == 0 || vinfo.yres == 0 || vinfo.xres > INT_MAX ||
        vinfo.yres > INT_MAX || vinfo.bits_per_pixel != 16 ||
        finfo.line_length > INT_MAX || page_bytes == 0 ||
        page_bytes > SIZE_MAX || finfo.smem_len < page_bytes) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                "framebuffer geometry changed to invalid layout after setup: "
                "%ux%u bpp=%u line_length=%u smem_len=%u\n",
                vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
                finfo.line_length, finfo.smem_len);
        goto fail;
    }
    display_log_fb_layout(&vinfo, &finfo, "active fb layout:");

    ctx->fb_w = (int)vinfo.xres;
    ctx->fb_h = (int)vinfo.yres;
    ctx->fb_bpp = (int)vinfo.bits_per_pixel;
    ctx->fb_line_length = (int)finfo.line_length;
    ctx->fb_size = finfo.smem_len;
    ctx->fb_page_bytes = (size_t)page_bytes;
    ctx->fb_yres_virtual = vinfo.yres_virtual;
    ctx->fb_page_count = double_buffer ? 2 : 1;
    ctx->fb_current_page = 0;
    ctx->fb_flip_enabled = double_buffer;
    ctx->fb_vsync_available = double_buffer ? 1 : -1;
    ctx->fb_var = vinfo;
    ctx->fb_fix = finfo;

    ctx->fb_base = mmap(NULL, ctx->fb_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, ctx->fb_fd, 0);
    if (ctx->fb_base == MAP_FAILED) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE, "mmap fb: %s\n", strerror(errno));
        ctx->fb_base = NULL;
        goto fail;
    }

    if (ctx->fb_flip_enabled && display_pan_to_page(ctx, 0) < 0) {
        /* 某些 BSP 虽报告 ypanstep，却不实现 pan；退化仍可保留显示功能。 */
        MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                "framebuffer page flip failed during setup; using staging buffer\n");
        ctx->fb_flip_enabled = 0;
        ctx->fb_page_count = 1;
        ctx->fb_vsync_available = -1;
    }
    if (!ctx->fb_flip_enabled) {
        ctx->fb_staging = calloc(1, ctx->fb_page_bytes);
        if (!ctx->fb_staging) {
            MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                    "alloc %zu-byte single-buffer staging frame failed\n",
                    ctx->fb_page_bytes);
            goto fail;
        }
        ctx->fb_staging_size = ctx->fb_page_bytes;
    }

    ctx->out_w = ctx->fb_w;
    ctx->out_h = ctx->fb_h;

    /*
     * 不在这里 memset 当前 framebuffer：page 0 可能正由 LCD 扫描，启动阶段
     * 清屏也会制造一次无意义的撕裂/黑帧。首帧会完整覆盖后台页，后续每次复用
     * 前还会用页指纹检查内容；因此无需用一次“直接写当前扫描页”换取清零。
     */
    MLOGI_M(IPCAM_DISPLAY_LOG_MODULE,
            "fb ready: %dx%d bpp=%d line_length=%d virtual_y=%u pages=%d "
            "page_bytes=%zu mode=%s size=%zu base=%p page0=%p page1=%p\n",
            ctx->fb_w, ctx->fb_h, ctx->fb_bpp, ctx->fb_line_length,
            ctx->fb_yres_virtual, ctx->fb_page_count,
            ctx->fb_page_bytes,
            ctx->fb_flip_enabled ? "pageflip-vsync" : "single-buffer-vsync",
            ctx->fb_size, ctx->fb_base, display_page(ctx, 0),
            ctx->fb_page_count > 1 ? display_page(ctx, 1) : NULL);
    return 0;

fail:
    display_release_fb(ctx);
    return -1;
}

static void *display_thread(void *arg)
{
    ipcam_display_ctx_t *ctx = arg;
    ipcam_frame_t frame;
    unsigned long frames = 0, skipped = 0, report_frames = 0;
    unsigned long present_errors = 0, vsync_waits = 0, flips = 0;
    uint64_t last_source_sequence = 0;
    int have_source_sequence = 0;
    int first_frame_logged = 0;
    struct timeval t0, t1, last_report, now;
    int dst_stride_pixels = ctx->fb_line_length / 2;  /* 16bpp = 2 bytes/pixel */

    MLOGI_M(IPCAM_DISPLAY_LOG_MODULE,
            "display thread start: src=%dx%d out=%dx%d mode=%s pages=%d\n",
            ctx->src_w, ctx->src_h, ctx->out_w, ctx->out_h,
            ctx->fb_flip_enabled ? "pageflip-vsync" : "single-buffer-vsync",
            ctx->fb_page_count);
    gettimeofday(&t0, NULL);
    last_report = t0;

    while (*ctx->running) {
        unsigned short *target;
        int target_page = 0;
        int src_w = ctx->src_w;
        int src_h = ctx->src_h;
        size_t expected = (size_t)src_w * src_h * 2;
        size_t src_stride_bytes;
        ipcam_frame_probe_t ring_probe;
        ipcam_frame_probe_t rgb_probe;

        if (ipcam_ring_get(ctx->rb, &frame) != 0) break;

        /*
         * capture 已按 bytesperline/sizeimage 严格过滤；display 再做一次边界检查，
         * 防止未来其它生产者把短帧送进来后，缩放函数跨行读取并显示半帧。
         */
        src_stride_bytes = frame.meta.source_bytesperline
            ? (size_t)frame.meta.source_bytesperline : (size_t)src_w * 2U;
        if (!frame.rawData || frame.size != expected ||
            src_stride_bytes < (size_t)src_w * 2U ||
            (frame.meta.source_frame_bytes != 0 &&
             frame.meta.source_frame_bytes != frame.size)) {
            skipped++;
            MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                    "frame ring_seq=%lu layout invalid: raw=%p size=%zu "
                    "expected=%zu source_frame_bytes=%u bpl=%zu, skip\n",
                    frame.seqNo, frame.rawData, frame.size, expected,
                    frame.meta.source_frame_bytes, src_stride_bytes);
            ipcam_ring_release(ctx->rb);
            continue;
        }

        if (frame.meta.source_sequence != 0) {
            if (have_source_sequence) {
                if (frame.meta.source_sequence > last_source_sequence + 1ULL)
                    ctx->source_sequence_gaps += (unsigned long)
                        (frame.meta.source_sequence - last_source_sequence - 1ULL);
                else if (frame.meta.source_sequence <= last_source_sequence)
                    ctx->source_sequence_rewinds++;
            }
            last_source_sequence = frame.meta.source_sequence;
            have_source_sequence = 1;
            ctx->last_source_sequence = frame.meta.source_sequence;
        }

        if (ipcam_frame_probe_pixels(frame.rawData, frame.size, src_w, src_h,
                                     2, src_stride_bytes, &ring_probe) != 0) {
            ctx->frame_probe_errors++;
            skipped++;
            MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                    "frame ring_seq=%lu source_seq=%llu probe failed; skip "
                    "(count=%lu)\n",
                    frame.seqNo,
                    (unsigned long long)frame.meta.source_sequence,
                    ctx->frame_probe_errors);
            ipcam_ring_release(ctx->rb);
            continue;
        }

        /*
         * 采集端把来源指纹随 payload 一起写入 ring；这里重新采样同一 slot。
         * 只要两者不一致，就说明问题发生在 ring 拷贝/槽位复用，而不是 LCD。
         */
        if (frame.meta.source_probe_global != 0 &&
            !display_probe_equal(&ring_probe,
                                 &(ipcam_frame_probe_t){
                                     .global = frame.meta.source_probe_global,
                                     .quadrant = {
                                         frame.meta.source_probe_quadrant[0],
                                         frame.meta.source_probe_quadrant[1],
                                         frame.meta.source_probe_quadrant[2],
                                         frame.meta.source_probe_quadrant[3]
                                     }
                                 })) {
            ctx->ring_hash_mismatch++;
            MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                    "ring payload mismatch: ring_seq=%lu source_seq=%llu "
                    "source=%08x/%08x/%08x/%08x/%08x actual=%08x/%08x/%08x/%08x/%08x "
                    "count=%lu\n",
                    frame.seqNo,
                    (unsigned long long)frame.meta.source_sequence,
                    frame.meta.source_probe_global,
                    frame.meta.source_probe_quadrant[0],
                    frame.meta.source_probe_quadrant[1],
                    frame.meta.source_probe_quadrant[2],
                    frame.meta.source_probe_quadrant[3],
                    ring_probe.global, ring_probe.quadrant[0],
                    ring_probe.quadrant[1], ring_probe.quadrant[2],
                    ring_probe.quadrant[3], ctx->ring_hash_mismatch);
            skipped++;
            ipcam_ring_release(ctx->rb);
            continue;
        }

        if (ctx->fb_flip_enabled) {
            target_page = ctx->fb_current_page ^ 1;
            target = display_page(ctx, target_page);

            if (ctx->fb_page_probe_valid[target_page]) {
                ipcam_frame_probe_t before;
                if (display_probe_page(ctx, target_page, &before) != 0) {
                    ctx->fb_probe_errors++;
                    MLOGW_M(IPCAM_DISPLAY_LOG_MODULE,
                            "fb page probe before reuse failed: page=%d "
                            "count=%lu\n", target_page, ctx->fb_probe_errors);
                } else if (!display_probe_equal(&before,
                                                &ctx->fb_page_probe[target_page])) {
                    ctx->fb_page_corrupt++;
                    MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                            "fb page changed while idle: page=%d addr=%p "
                            "expected=%08x actual=%08x count=%lu\n",
                            target_page, (void *)target,
                            ctx->fb_page_probe[target_page].global,
                            before.global, ctx->fb_page_corrupt);
                }
            }
        } else {
            target = ctx->fb_staging;
        }
        if (!target) {
            present_errors++;
            MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                    "display target buffer unavailable; stopping display thread\n");
            ipcam_ring_release(ctx->rb);
            if (ctx->running) *ctx->running = 0;
            break;
        }

        yuv422_packed_to_rgb565_scaled(frame.rawData, src_w, src_h,
                                       src_stride_bytes,
                                       target, ctx->out_w, ctx->out_h,
                                       dst_stride_pixels);

        if (ipcam_frame_probe_pixels(target, ctx->fb_page_bytes,
                                     ctx->out_w, ctx->out_h, 2,
                                     (size_t)ctx->fb_line_length,
                                     &rgb_probe) != 0) {
            ctx->fb_probe_errors++;
            skipped++;
            MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                    "rgb565 target probe failed: page=%d count=%lu\n",
                    target_page, ctx->fb_probe_errors);
            ipcam_ring_release(ctx->rb);
            continue;
        }
        ctx->last_source_probe = ring_probe.global;
        ctx->last_rgb_probe = rgb_probe.global;
        if (ctx->fb_flip_enabled) {
            ctx->fb_page_probe[target_page] = rgb_probe;
            ctx->fb_page_probe_valid[target_page] = 1;
        }

        /* 转换已经完成，先释放 ring 槽，再等待 VSYNC/翻页，减少采集侧背压。 */
        ipcam_ring_release(ctx->rb);

        if (ctx->fb_flip_enabled) {
            if (display_pan_to_page(ctx, target_page) < 0) {
                present_errors++;
                if (ctx->running) *ctx->running = 0;
                break;
            }
            flips++;
        } else {
            if (display_wait_for_vsync(ctx) == 0)
                vsync_waits++;
            /* VSYNC 不可用时仍复制完整 staging 帧，保留“能显示”的最后退化能力。 */
            memcpy(ctx->fb_base, ctx->fb_staging, ctx->fb_page_bytes);
        }
        frames++;

        /* BCF2 的显示线程记录首帧，用于确认采集、转换和 LCD 提交均已走通。 */
        if (!first_frame_logged) {
            MLOGI_M(IPCAM_DISPLAY_LOG_MODULE,
                    "first frame: ring_seq=%lu source_seq=%llu buf=%u "
                    "bytes=%zu bpl=%u timestamp=%llu src=%dx%d "
                    "source_probe=%08x ring_probe=%08x "
                    "rgb_probe=%08x presented=%s page=%d yoffset=%u\n",
                    frame.seqNo,
                    (unsigned long long)frame.meta.source_sequence,
                    frame.meta.source_buffer_index, frame.size,
                    frame.meta.source_bytesperline,
                    (unsigned long long)frame.meta.source_timestamp_us,
                    src_w, src_h,
                    frame.meta.source_probe_global, ring_probe.global,
                    rgb_probe.global,
                    ctx->fb_flip_enabled ? "pageflip" : "copy",
                    ctx->fb_flip_enabled ? ctx->fb_current_page : 0,
                    ctx->fb_var.yoffset);
            first_frame_logged = 1;
            display_log_probe("first ring", &ring_probe);
            display_log_probe("first rgb565", &rgb_probe);
        } else if ((frames % 30UL) == 0) {
            MLOGD_M(IPCAM_DISPLAY_LOG_MODULE,
                    "frame probe: ring_seq=%lu source_seq=%llu buf=%u page=%d "
                    "addr=%p source=%08x ring=%08x rgb=%08x yoffset=%u\n",
                    frame.seqNo,
                    (unsigned long long)frame.meta.source_sequence,
                    frame.meta.source_buffer_index,
                    ctx->fb_flip_enabled ? ctx->fb_current_page : 0,
                    (void *)target, frame.meta.source_probe_global,
                    ring_probe.global, rgb_probe.global, ctx->fb_var.yoffset);
        }

        /* 每 5 秒统计一次，避免按帧打印拖慢单核 i.MX6ULL 的显示调度。 */
        gettimeofday(&now, NULL);
        double report_sec = (now.tv_sec - last_report.tv_sec) +
                            (now.tv_usec - last_report.tv_usec) / 1e6;
        if (report_sec >= 5.0) {
            unsigned long interval_frames = frames - report_frames;
            if (display_verify_fb_runtime(ctx) < 0) {
                present_errors++;
                MLOGE_M(IPCAM_DISPLAY_LOG_MODULE,
                        "framebuffer runtime verification failed; stopping display\n");
                if (ctx->running) *ctx->running = 0;
                break;
            }
            MLOGI_M(IPCAM_DISPLAY_LOG_MODULE,
                    "stats: interval=%.1fs fps=%.1f frames=%lu skipped=%lu "
                    "present_errors=%lu flips=%lu vsync_waits=%lu "
                    "pan_us=%llu/%llu/%llu slow_pan=%lu "
                    "diag={ring_mismatch=%lu page_corrupt=%lu fb_layout=%lu "
                    "pan_error=%lu pan_timeout=%lu bad_yoffset=%lu "
                    "fb_query=%lu fb_probe=%lu frame_probe=%lu "
                    "src_gap=%lu src_rewind=%lu} "
                    "last={src=%llu source_probe=%08x rgb_probe=%08x yoff=%u} rb=%d\n",
                    report_sec,
                    report_sec > 0 ? interval_frames / report_sec : 0,
                    frames, skipped, present_errors, flips, vsync_waits,
                    ctx->pan_calls ? ctx->pan_total_us / ctx->pan_calls : 0ULL,
                    ctx->pan_min_us, ctx->pan_max_us,
                    ctx->pan_wait_over_threshold,
                    ctx->ring_hash_mismatch, ctx->fb_page_corrupt,
                    ctx->fb_layout_changes, ctx->pan_errors,
                    ctx->pan_timeouts, ctx->unexpected_yoffset,
                    ctx->fb_query_errors, ctx->fb_probe_errors,
                    ctx->frame_probe_errors, ctx->source_sequence_gaps,
                    ctx->source_sequence_rewinds,
                    (unsigned long long)ctx->last_source_sequence,
                    ctx->last_source_probe, ctx->last_rgb_probe,
                    ctx->fb_var.yoffset,
                    ipcam_ring_count(ctx->rb));
            if (ctx->fb_flip_enabled) {
                for (int page = 0; page < ctx->fb_page_count; page++) {
                    MLOGD_M(IPCAM_DISPLAY_LOG_MODULE,
                            "fb page[%d]: addr=%p valid=%d probe=%08x\n",
                            page, (void *)display_page(ctx, page),
                            ctx->fb_page_probe_valid[page],
                            ctx->fb_page_probe[page].global);
                }
            }
            last_report = now;
            report_frames = frames;
        }
    }

    gettimeofday(&t1, NULL);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    MLOGI_M(IPCAM_DISPLAY_LOG_MODULE,
            "display thread exit, frames=%lu skipped=%lu present_errors=%lu "
            "flips=%lu vsync_waits=%lu avg_fps=%.1f "
            "diag={ring_mismatch=%lu page_corrupt=%lu fb_layout=%lu "
            "pan_error=%lu pan_timeout=%lu bad_yoffset=%lu fb_query=%lu "
            "fb_probe=%lu frame_probe=%lu src_gap=%lu src_rewind=%lu}\n",
            frames, skipped, present_errors, flips, vsync_waits,
            sec > 0 ? frames / sec : 0,
            ctx->ring_hash_mismatch, ctx->fb_page_corrupt,
            ctx->fb_layout_changes, ctx->pan_errors, ctx->pan_timeouts,
            ctx->unexpected_yoffset, ctx->fb_query_errors,
            ctx->fb_probe_errors, ctx->frame_probe_errors,
            ctx->source_sequence_gaps, ctx->source_sequence_rewinds);
    return NULL;
}

int ipcam_display_start(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                        int src_w, int src_h,
                        volatile sig_atomic_t *running)
{
    if (!ctx || !rb || !running) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->fb_fd = -1;
    ctx->fb_base = NULL;
    ctx->rb = rb;
    ctx->running = running;
    ctx->src_w = src_w > 0 ? src_w : IPCAM_CAPTURE_WIDTH;
    ctx->src_h = src_h > 0 ? src_h : IPCAM_CAPTURE_HEIGHT;

    if (display_open_fb(ctx) < 0) return -1;

    if (pthread_create(&ctx->thread, NULL, display_thread, ctx) != 0) {
        MLOGE_M(IPCAM_DISPLAY_LOG_MODULE, "pthread_create display failed\n");
        display_release_fb(ctx);
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

    display_release_fb(ctx);
}
