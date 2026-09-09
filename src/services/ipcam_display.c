#define _GNU_SOURCE
/* LCD/framebuffer、预览视口和本地渲染日志归入 DISP 模块。 */
#define IPCAM_LOG_MODULE "DISP"
#include "ipcam_display.h"
#include "ipcam_log.h"
#include "ipcam_param.h"

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
#include <sys/stat.h>
#include <stdint.h>
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
/* 依据视口和独立翻转选样，只写目标 framebuffer 区域，不改网络/录像帧。 */
static void yuyv_to_rgb565_scaled(const unsigned char *src, int sw, int sh,
                                  unsigned short *dst, int dw, int dh,
                                  int dst_stride_pixels,
                                  size_t src_stride,
                                  int crop_x, int crop_y, int crop_w, int crop_h,
                                  int mirror_h, int mirror_v)
{
    if (sw < 2) return;  /* YUYV 4:2:2 需要至少 2 像素宽 */
    for (int dy = 0; dy < dh; dy++) {
        int cy = dy * crop_h / dh;
        int sy = mirror_v ? (crop_y + crop_h - 1 - cy) : (crop_y + cy);
        const unsigned char *src_row = src + (size_t)sy * src_stride;
        unsigned short *dst_row = dst + (size_t)dy * dst_stride_pixels;
        for (int dx = 0; dx < dw; dx++) {
            int cx = dx * crop_w / dw;
            int sx = mirror_h ? (crop_x + crop_w - 1 - cx) : (crop_x + cx);
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

/* 打开并核验 framebuffer；实际节点由板级环境变量提供，失败只停本地预览。 */
static int display_open_fb(ipcam_display_ctx_t *ctx)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    const char *fb_dev = getenv("IPCAM_FB_DEV");
    if (!fb_dev || !*fb_dev) fb_dev = IPCAM_FB_DEV;
    ctx->fb_fd = open(fb_dev, O_RDWR);
    if (ctx->fb_fd < 0) {
        MLOGE("open %s: %s\n", fb_dev, strerror(errno));
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

    /* framebuffer 的可见窗口可能位于 virtual buffer 的偏移位置；先核验
     * 虚拟尺寸和 offset，再让 LVGL flush 使用这些 offset，避免合法的局部
     * 刷新因驱动参数异常写出 mmap 区域。 */
    if (vinfo.xres == 0 || vinfo.yres == 0 ||
        vinfo.xres_virtual < vinfo.xres ||
        vinfo.yres_virtual < vinfo.yres ||
        vinfo.xoffset > vinfo.xres_virtual - vinfo.xres ||
        vinfo.yoffset > vinfo.yres_virtual - vinfo.yres) {
        MLOGE("invalid fb geometry: visible=%ux%u virtual=%ux%u offset=%u,%u\n",
              vinfo.xres, vinfo.yres, vinfo.xres_virtual, vinfo.yres_virtual,
              vinfo.xoffset, vinfo.yoffset);
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        return -1;
    }

    ctx->fb_w  = vinfo.xres;
    ctx->fb_h  = vinfo.yres;
    ctx->fb_bpp = vinfo.bits_per_pixel;
    ctx->fb_xoffset = (int)vinfo.xoffset;
    ctx->fb_yoffset = (int)vinfo.yoffset;
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
    uint64_t visible_end = ((uint64_t)ctx->fb_yoffset + (uint64_t)ctx->fb_h) *
                           (uint64_t)ctx->fb_line_length;
    if (visible_end > (uint64_t)ctx->fb_size) {
        MLOGE("fb memory=%zu below offset framebuffer end=%llu\n", ctx->fb_size,
              (unsigned long long)visible_end);
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

/* 只读取显示专用最新帧副本；关闭预览/熄屏时停止转换，避免拖慢编码链路。 */
static void *display_thread(void *arg)
{
    ipcam_display_ctx_t *ctx = arg;
    ipcam_frame_t frame;
    unsigned long frames = 0;
    struct timeval t0, t1;
    size_t src_cap = ipcam_ring_capacity(ctx->rb);
    unsigned char *src_copy = src_cap ? malloc(src_cap) : NULL;
    if (!src_copy) {
        MLOGE("alloc display source copy failed (%zu bytes)\n", src_cap);
        return NULL;
    }
    MLOGI("display thread start, out=%dx%d\n", ctx->out_w, ctx->out_h);
    gettimeofday(&t0, NULL);

    int last_enabled = -1;
    unsigned long last_seq = 0;
    while (*ctx->running && ctx->service_running) {
        int enabled;
        int view_flag;
        int paused_flag;
        float zoom, center_x, center_y;
        pthread_mutex_lock(&ctx->view_mtx);
        view_flag = ctx->view_enabled;
        paused_flag = ctx->screen_paused;
        enabled = view_flag && !paused_flag &&
                  ipcam_param_get_preview_enabled();
        zoom = ctx->zoom;
        center_x = ctx->center_x;
        center_y = ctx->center_y;
        pthread_mutex_unlock(&ctx->view_mtx);
        if (!enabled) {
            /* 预览关闭/熄屏时不复制或转换 YUYV；只在状态边沿清一次屏。 */
            if (last_enabled != 0)
                MLOGI("preview inactive: view=%d screen_paused=%d\n",
                      view_flag, paused_flag);
            if (last_enabled != 0 && ctx->framebuffer_writer_enabled)
                memset(ctx->fb_base, 0, ctx->fb_size);
            pthread_mutex_lock(&ctx->preview_mtx);
            ctx->preview_valid = 0;
            pthread_mutex_unlock(&ctx->preview_mtx);
            last_enabled = 0;
            usleep(50 * 1000);
            continue;
        }
        if (last_enabled != 1)
            MLOGI("preview active: zoom=%.2f center=%.3f,%.3f writer=%d\n",
                  zoom, center_x, center_y, ctx->framebuffer_writer_enabled);
        last_enabled = 1;

        int latest_rc = ipcam_ring_copy_latest(ctx->rb, src_copy, src_cap,
                                               &frame, last_seq);
        if (latest_rc != 0) {
            /* copy_latest 是非阻塞查询；短暂让出 CPU，避免无帧时忙等。 */
            usleep(latest_rc < 0 ? 50 * 1000 : 10 * 1000);
            continue;
        }
        last_seq = frame.seqNo;
        frame.rawData = src_copy;

        /*
         * src 宽高由 capture 协商结果传入（ctx->src_w / src_h）。
         * V4L2 的 bytesperline 可能带行尾 padding，不能再用
         * src_w*src_h*2 做精确比较；只要完整覆盖 stride*height 即可安全读取。
         */
        int src_w = ctx->src_w;
        int src_h = ctx->src_h;
        size_t src_stride = frame.stride ? frame.stride : (size_t)src_w * 2;
        size_t expected = src_stride * (size_t)src_h;
        if (src_stride < (size_t)src_w * 2 || frame.size < expected) {
            MLOGW("frame size %zu/stride %zu invalid for %dx%d (%zu), skip\n",
                  frame.size, src_stride, src_w, src_h, expected);
            continue;
        }

        if (zoom < 1.0f) zoom = 1.0f;
        if (zoom > 4.0f) zoom = 4.0f;
        int crop_w = (int)((float)src_w / zoom);
        int crop_h = (int)((float)src_h / zoom);
        /* 先按倍率确定观察窗口，再按 LCD 宽高比收窄一个方向；如果直接
         * 把 4:3 源图拉伸到 1024:600，双指缩放看似可用但物体比例会变形。 */
        float src_ratio = (float)crop_w / (float)(crop_h > 0 ? crop_h : 1);
        float dst_ratio = (float)ctx->out_w / (float)(ctx->out_h > 0 ? ctx->out_h : 1);
        if (src_ratio > dst_ratio)
            crop_w = (int)((float)crop_h * dst_ratio);
        else if (src_ratio < dst_ratio)
            crop_h = (int)((float)crop_w / dst_ratio);
        if (crop_w < 2) crop_w = 2;
        if (crop_h < 2) crop_h = 2;
        /* YUYV 色度按像素对采样，水平窗口保持偶数，避免边缘半对错位。 */
        if (crop_w & 1) crop_w--;
        if (crop_w < 2) crop_w = 2;
        int crop_x = (int)(center_x * src_w - crop_w / 2);
        int crop_y = (int)(center_y * src_h - crop_h / 2);
        if (crop_x < 0) crop_x = 0;
        if (crop_y < 0) crop_y = 0;
        if (crop_x + crop_w > src_w) crop_x = src_w - crop_w;
        if (crop_y + crop_h > src_h) crop_y = src_h - crop_h;
        /* 先渲染到独立预览缓冲，再按 framebuffer 行跨度提交；这样 GUI
         * 可在不持有 ring 槽的情况下复制同一帧，媒体线程也不会直接覆盖整屏。 */
        pthread_mutex_lock(&ctx->preview_mtx);
        yuyv_to_rgb565_scaled(frame.rawData, src_w, src_h,
                              ctx->preview_base, ctx->out_w, ctx->out_h, ctx->out_w,
                              src_stride,
                              crop_x, crop_y, crop_w, crop_h,
                              ipcam_param_get_mirror_horizontal(),
                              ipcam_param_get_mirror_vertical());
        if (ctx->framebuffer_writer_enabled) {
            for (int y = 0; y < ctx->out_h; y++) {
                memcpy((unsigned char *)ctx->fb_base +
                           (size_t)(y + ctx->fb_yoffset) * ctx->fb_line_length +
                           (size_t)ctx->fb_xoffset * sizeof(*ctx->preview_base),
                       ctx->preview_base + (size_t)y * ctx->out_w,
                       (size_t)ctx->out_w * sizeof(*ctx->preview_base));
            }
        }
        ctx->preview_frame = frame;
        ctx->preview_frame.rawData = ctx->preview_base;
        ctx->preview_frame.size = ctx->preview_size;
        ctx->preview_frame.width = (uint16_t)ctx->out_w;
        ctx->preview_frame.height = (uint16_t)ctx->out_h;
        ctx->preview_frame.stride = (uint32_t)ctx->out_w * 2U;
        ctx->preview_frame.pixel_format = IPCAM_PIXEL_FORMAT_RGB565;
        ctx->preview_valid = 1;
        pthread_mutex_unlock(&ctx->preview_mtx);
        pthread_mutex_lock(&ctx->stats_mtx);
        ctx->frames_rendered++;
        pthread_mutex_unlock(&ctx->stats_mtx);
        frames++;
        /* 与 BCF2 视频线程一致，首批帧和周期帧带上源序号/时间戳，便于把 LCD
         * 卡顿与 CSI、颜色转换或 framebuffer 提交区分开。 */
        if (frames <= 30 || (frames % 30) == 0) {
            MLOGI("frame no=%lu src_seq=%lu ts=%llu src=%dx%d crop=%dx%d+%d+%d\n",
                  frames, frame.seqNo, (unsigned long long)frame.monotonic_ns,
                  src_w, src_h, crop_w, crop_h, crop_x, crop_y);
        }
    }

    gettimeofday(&t1, NULL);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    MLOGI("display thread exit, frames=%lu avg_fps=%.1f\n",
          frames, sec > 0 ? frames / sec : 0);
    free(src_copy);
    return NULL;
}

/*
 * 打开 framebuffer 并启动本地处理线程；失败只影响预览，不停止采集。
 * framebuffer_writer=0 时仍映射 framebuffer 以供 LVGL flush 复用，但显示线程
 * 只维护独立 RGB565 预览副本，避免两个线程同时写 LCD 造成撕裂和控件消失。
 */
int ipcam_display_start_ex(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                           int src_w, int src_h, int framebuffer_writer,
                           volatile sig_atomic_t *running)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->fb_fd = -1;
    ctx->fb_base = NULL;
    ctx->rb = rb;
    ctx->running = running;
    ctx->service_running = 1;
    ctx->framebuffer_writer_enabled = framebuffer_writer ? 1 : 0;
    ctx->src_w = src_w > 0 ? src_w : IPCAM_CAPTURE_WIDTH;
    ctx->src_h = src_h > 0 ? src_h : IPCAM_CAPTURE_HEIGHT;
    pthread_mutex_init(&ctx->view_mtx, NULL);
    pthread_mutex_init(&ctx->preview_mtx, NULL);
    pthread_mutex_init(&ctx->stats_mtx, NULL);
    ctx->view_enabled = ipcam_param_get_preview_enabled() ? 1 : 0;
    ctx->zoom = 1.0f;
    ctx->center_x = 0.5f;
    ctx->center_y = 0.5f;

    if (display_open_fb(ctx) < 0) {
        pthread_mutex_destroy(&ctx->stats_mtx);
        pthread_mutex_destroy(&ctx->preview_mtx);
        pthread_mutex_destroy(&ctx->view_mtx);
        return -1;
    }
    if (ctx->out_w <= 0 || ctx->out_h <= 0 ||
        (size_t)ctx->out_w > SIZE_MAX / (size_t)ctx->out_h / sizeof(*ctx->preview_base)) {
        MLOGE("invalid framebuffer dimensions %dx%d\n", ctx->out_w, ctx->out_h);
        munmap(ctx->fb_base, ctx->fb_size);
        ctx->fb_base = NULL;
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        pthread_mutex_destroy(&ctx->stats_mtx);
        pthread_mutex_destroy(&ctx->preview_mtx);
        pthread_mutex_destroy(&ctx->view_mtx);
        return -1;
    }
    ctx->preview_size = (size_t)ctx->out_w * (size_t)ctx->out_h * sizeof(*ctx->preview_base);
    ctx->preview_base = calloc(1, ctx->preview_size);
    if (!ctx->preview_base) {
        MLOGE("alloc preview RGB565 buffer failed (%zu bytes)\n", ctx->preview_size);
        munmap(ctx->fb_base, ctx->fb_size);
        ctx->fb_base = NULL;
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        pthread_mutex_destroy(&ctx->stats_mtx);
        pthread_mutex_destroy(&ctx->preview_mtx);
        pthread_mutex_destroy(&ctx->view_mtx);
        return -1;
    }
    /* 背光接口是板级可选项；没有导出的 sysfs 节点时保留视频服务并记录一次警告。 */
    if (ipcam_display_set_backlight_percent(ipcam_param_get_backlight_percent()) != 0) {
        MLOGW("backlight capability unavailable; set IPCAM_BACKLIGHT_PATH after board probing\n");
    }

    if (pthread_create(&ctx->thread, NULL, display_thread, ctx) != 0) {
        MLOGE("pthread_create display failed\n");
        if (ctx->fb_base) munmap(ctx->fb_base, ctx->fb_size);
        if (ctx->fb_fd >= 0) close(ctx->fb_fd);
        free(ctx->preview_base);
        ctx->preview_base = NULL;
        pthread_mutex_destroy(&ctx->stats_mtx);
        pthread_mutex_destroy(&ctx->view_mtx);
        pthread_mutex_destroy(&ctx->preview_mtx);
        return -1;
    }
    MLOGI("display service ready: fb=%dx%d src=%dx%d writer=%d view=%d\n",
          ctx->out_w, ctx->out_h, ctx->src_w, ctx->src_h,
          ctx->framebuffer_writer_enabled, ctx->view_enabled);
    return 0;
}

/* 旧接口默认由 display 线程直接写屏，保留无 LVGL 构建/调试程序的行为。 */
int ipcam_display_start(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                        int src_w, int src_h,
                        volatile sig_atomic_t *running)
{
    return ipcam_display_start_ex(ctx, rb, src_w, src_h, 1, running);
}

/* 停止本地转换并释放 framebuffer，保持全局采集/编码运行标志不变。 */
void ipcam_display_stop(ipcam_display_ctx_t *ctx)
{
    if (!ctx) return;
    uint64_t rendered = 0;
    ipcam_display_get_stats(ctx, &rendered);
    MLOGI("display stop requested: rendered=%llu\n",
          (unsigned long long)rendered);
    /* 仅停止显示服务；不能修改 main 的全局运行标志，否则关闭 LCD
     * 会连带终止采集、编码和网络服务。关闭输入 ring 负责唤醒线程。 */
    ctx->service_running = 0;
    if (ctx->rb) ipcam_ring_close(ctx->rb);
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
    free(ctx->preview_base);
    ctx->preview_base = NULL;
    ctx->preview_size = 0;
    pthread_mutex_destroy(&ctx->preview_mtx);
    pthread_mutex_destroy(&ctx->view_mtx);
    pthread_mutex_destroy(&ctx->stats_mtx);
    MLOGI("display stopped\n");
}

/* 运行期切换 framebuffer 所有者；只在 LVGL 启动失败的回退分支调用。 */
void ipcam_display_set_framebuffer_writer(ipcam_display_ctx_t *ctx, int enabled)
{
    if (!ctx) return;
    ctx->framebuffer_writer_enabled = enabled ? 1 : 0;
}

/* 原子替换观察视口；中心坐标归一化到 0～1，zoom 限制在 1～4。 */
int ipcam_display_set_view(ipcam_display_ctx_t *ctx, int enabled,
                           float zoom, float center_x, float center_y)
{
    if (!ctx || zoom < 1.0f || zoom > 4.0f || center_x < 0.0f || center_x > 1.0f ||
        center_y < 0.0f || center_y > 1.0f) return -1;
    pthread_mutex_lock(&ctx->view_mtx);
    ctx->view_enabled = enabled ? 1 : 0;
    ctx->zoom = zoom;
    ctx->center_x = center_x;
    ctx->center_y = center_y;
    pthread_mutex_unlock(&ctx->view_mtx);
    MLOGI("view changed: enabled=%d zoom=%.2f center=%.3f,%.3f\n",
          enabled ? 1 : 0, zoom, center_x, center_y);
    return 0;
}

/* 熄屏只暂停本地转换，保留 zoom/中心，唤醒后无需重置 GUI 状态。 */
int ipcam_display_set_screen_paused(ipcam_display_ctx_t *ctx, int paused)
{
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->view_mtx);
    ctx->screen_paused = paused ? 1 : 0;
    pthread_mutex_unlock(&ctx->view_mtx);
    MLOGI("screen preview %s\n", paused ? "paused" : "resumed");
    return 0;
}

/* 通过板级 sysfs 节点设置亮度；未配置节点时返回失败而不伪造成功。 */
int ipcam_display_set_backlight_percent(int percent)
{
    if (percent < 0 || percent > 100) return -1;
    const char *path = getenv("IPCAM_BACKLIGHT_PATH");
    if (!path || !*path) return -1;
    int max_value = 100;
    const char *max_env = getenv("IPCAM_BACKLIGHT_MAX");
    if (max_env && *max_env) max_value = atoi(max_env);
    if (max_value <= 0) return -1;
    int value = max_value * percent / 100;
    FILE *fp = fopen(path, "w");
    if (!fp) { MLOGW("backlight open %s: %s\n", path, strerror(errno)); return -1; }
    int write_ok = fprintf(fp, "%d\n", value) > 0;
    int close_ok = fclose(fp) == 0;
    int rc = write_ok && close_ok ? 0 : -1;
    if (rc != 0) MLOGW("backlight write %s failed\n", path);
    else MLOGI("backlight set: percent=%d value=%d path=%s\n", percent, value, path);
    return rc;
}

/* 复制当前视口快照，供控制器构造命令时保留 zoom/中心。 */
void ipcam_display_get_view(ipcam_display_ctx_t *ctx, int *enabled,
                            float *zoom, float *center_x, float *center_y)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->view_mtx);
    if (enabled) *enabled = ctx->view_enabled;
    if (zoom) *zoom = ctx->zoom;
    if (center_x) *center_x = ctx->center_x;
    if (center_y) *center_y = ctx->center_y;
    pthread_mutex_unlock(&ctx->view_mtx);
}

/* 复制渲染帧计数；统计锁与 framebuffer/视口锁分离，避免查询拖慢转换线程。 */
void ipcam_display_get_stats(ipcam_display_ctx_t *ctx, uint64_t *rendered)
{
    if (!ctx || !rendered) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    *rendered = ctx->frames_rendered;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/* 复制最新 RGB565 预览帧；复制期间锁住工作缓冲，调用方无需归还槽位。 */
int ipcam_display_preview_acquire(ipcam_display_ctx_t *ctx,
                                  void *out_data, size_t out_cap,
                                  ipcam_frame_t *out_frame)
{
    if (!ctx || !out_data || !out_frame) return -1;
    pthread_mutex_lock(&ctx->preview_mtx);
    if (!ctx->preview_valid || ctx->preview_size > out_cap) {
        pthread_mutex_unlock(&ctx->preview_mtx);
        return -1;
    }
    memcpy(out_data, ctx->preview_base, ctx->preview_size);
    *out_frame = ctx->preview_frame;
    out_frame->rawData = out_data;
    pthread_mutex_unlock(&ctx->preview_mtx);
    return 0;
}

void ipcam_display_preview_release(ipcam_display_ctx_t *ctx, ipcam_frame_t *frame)
{
    /* acquire 是复制语义，保留成对 API 供未来零拷贝实现，不释放调用方内存。 */
    (void)ctx;
    (void)frame;
}
