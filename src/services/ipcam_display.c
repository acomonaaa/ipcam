#define _GNU_SOURCE
/* LCD/framebuffer、预览视口和本地渲染日志归入 DISP 模块。 */
#define IPCAM_LOG_MODULE "DISP"
#include "ipcam_display.h"
#include "ipcam_log.h"
#include "ipcam_param.h"

#include <errno.h>
#include <dirent.h>
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

#define IPCAM_BACKLIGHT_ROOT "/sys/class/backlight"

/* 当前产品只有一个 LCD 实例；旧的无 ctx 背光 API 用它保持 ABI 兼容。 */
static ipcam_display_ctx_t *s_default_display;

/*
 * YUYV 颜色转换的亮度项和色度项只依赖单个 8 位分量；预先拆成查表项后，
 * 640×480 源图每帧可省掉数百万次乘法。表格很小，启动时一次初始化，不增加
 * 预览帧缓冲占用，也不会改变 RGB565 的截断和饱和规则。
 */
static int s_y_base[256];
static int s_r_v[256];
static int s_g_u[256];
static int s_g_v[256];
static int s_b_u[256];
static pthread_once_t s_yuyv_lut_once = PTHREAD_ONCE_INIT;

static void yuyv_init_lut(void)
{
    for (int value = 0; value < 256; value++) {
        int c = value - 16;
        if (c < 0) c = 0;
        s_y_base[value] = 298 * c + 128;
        s_r_v[value] = 409 * (value - 128);
        s_g_u[value] = -100 * (value - 128);
        s_g_v[value] = -208 * (value - 128);
        s_b_u[value] = 516 * (value - 128);
    }
}

/* YCbCr -> RGB ITU-R BT.601 近似，输出 RGB565（5:6:5） */
static inline unsigned short yuyv_to_rgb565(int y, int u, int v)
{
    int r = (s_y_base[y] + s_r_v[v]) >> 8;
    int g = (s_y_base[y] + s_g_u[u] + s_g_v[v]) >> 8;
    int b = (s_y_base[y] + s_b_u[u]) >> 8;
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
    if (!src || !dst || sw < 2 || sh <= 0 || dw <= 0 || dh <= 0 ||
        crop_w < 2 || crop_h < 1 || dst_stride_pixels < dw)
        return;  /* YUYV 4:2:2 需要至少 2 像素宽 */

    /* 原实现把两次整数除法放在每个目标像素内循环；P03 每帧约 28 万
     * 像素，在 396MHz i.MX6ULL 上会直接把 LCD 帧率压低。映射表只在一帧
     * 开始时计算一次，内层只保留指针加法和颜色转换，且仍保持最近邻语义。 */
    uint32_t x_pair_offset[dw];
    uint8_t x_luma_offset[dw];
    int y_source[dh];
    for (int dx = 0; dx < dw; dx++) {
        int cx = dx * crop_w / dw;
        int sx = mirror_h ? (crop_x + crop_w - 1 - cx) : (crop_x + cx);
        int sx0 = sx & ~1;
        if (sx0 >= sw - 1) sx0 = sw - 2;
        if (sx0 < 0) sx0 = 0;
        x_pair_offset[dx] = (uint32_t)(sx0 * 2);
        x_luma_offset[dx] = (uint8_t)((sx & 1) ? 2 : 0);
    }
    for (int dy = 0; dy < dh; dy++) {
        int cy = dy * crop_h / dh;
        y_source[dy] = mirror_v ? (crop_y + crop_h - 1 - cy) : (crop_y + cy);
    }

    for (int dy = 0; dy < dh; dy++) {
        const unsigned char *src_row = src + (size_t)y_source[dy] * src_stride;
        unsigned short *dst_row = dst + (size_t)dy * dst_stride_pixels;
        for (int dx = 0; dx < dw; dx++) {
            const unsigned char *pair = src_row + x_pair_offset[dx];
            /* YUYV 每两像素一对 (Cb, Cr)，映射表已保证 pair 不越过行尾。 */
            dst_row[dx] = yuyv_to_rgb565(pair[x_luma_offset[dx]],
                                         pair[1], pair[3]);
        }
    }
}

/* 读取 sysfs 的整数节点；缺失或内容非法时返回 -1，调用方再决定回退策略。 */
static int read_int_file(const char *path)
{
    if (!path || !*path) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int value = -1;
    if (fscanf(fp, "%d", &value) != 1) value = -1;
    fclose(fp);
    return value;
}

/* 从 brightness 节点旁边读取 max_brightness，避免把 100 错当成 PWM 的上限。 */
static int backlight_read_max(const char *brightness_path)
{
    if (!brightness_path || !*brightness_path) return -1;
    char max_path[sizeof(((ipcam_display_ctx_t *)0)->backlight_path)];
    snprintf(max_path, sizeof(max_path), "%s", brightness_path);
    char *slash = strrchr(max_path, '/');
    if (!slash) return -1;
    snprintf(slash + 1, (size_t)(max_path + sizeof(max_path) - slash - 1),
             "max_brightness");
    return read_int_file(max_path);
}

/*
 * 探测背光控制能力。
 * IPCAM_BACKLIGHT_PATH 优先，未设置时扫描 pwm-backlight 等标准 sysfs 节点；
 * 这样当前板级设备树即使没有预置环境变量，也能正确响应亮度和休眠操作。
 */
static void display_probe_backlight(ipcam_display_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->backlight_path[0] = '\0';
    ctx->backlight_max = 0;
    ctx->backlight_available = 0;
    /* 先用一次无副作用的 unblank 探测备用能力；真正熄屏时只调用同一 ioctl。 */
    ctx->fb_blank_available = ioctl(ctx->fb_fd, FBIOBLANK, FB_BLANK_UNBLANK) == 0;
    if (!ctx->fb_blank_available)
        MLOGW("FBIOBLANK unavailable: %s\n", strerror(errno));

    const char *configured = getenv("IPCAM_BACKLIGHT_PATH");
    if (configured && *configured && access(configured, F_OK) == 0)
        snprintf(ctx->backlight_path, sizeof(ctx->backlight_path), "%s", configured);

    if (!ctx->backlight_path[0]) {
        DIR *dir = opendir(IPCAM_BACKLIGHT_ROOT);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                char candidate[sizeof(ctx->backlight_path)];
                int n = snprintf(candidate, sizeof(candidate), "%s/%s/brightness",
                                 IPCAM_BACKLIGHT_ROOT, entry->d_name);
                if (n <= 0 || (size_t)n >= sizeof(candidate) ||
                    access(candidate, F_OK) != 0)
                    continue;
                snprintf(ctx->backlight_path, sizeof(ctx->backlight_path), "%s",
                         candidate);
                break;
            }
            closedir(dir);
        }
    }

    if (!ctx->backlight_path[0]) {
        MLOGW("backlight sysfs node not found; FBIOBLANK will be used for sleep\n");
        return;
    }

    const char *max_env = getenv("IPCAM_BACKLIGHT_MAX");
    if (max_env && *max_env) ctx->backlight_max = atoi(max_env);
    if (ctx->backlight_max <= 0) ctx->backlight_max = backlight_read_max(ctx->backlight_path);
    if (ctx->backlight_max <= 0) ctx->backlight_max = 100;
    ctx->backlight_available = 1;
    MLOGI("backlight ready: path=%s max=%d%s\n", ctx->backlight_path,
          ctx->backlight_max,
          configured && *configured ? " source=env" : " source=auto");
}

/* 只清理实际虚拟页，避免原实现把驱动保留的 32 MiB 映射区整段写零。 */
static void display_clear_framebuffer(ipcam_display_ctx_t *ctx)
{
    if (!ctx || !ctx->fb_base) return;
    pthread_mutex_lock(&ctx->fb_mtx);
    int pages = ctx->fb_pan_enabled ? 2 : 1;
    for (int page = 0; page < pages; page++) {
        size_t offset = (size_t)page * ctx->fb_page_size;
        if (offset >= ctx->fb_size) break;
        size_t length = ctx->fb_page_size;
        if (length > ctx->fb_size - offset) length = ctx->fb_size - offset;
        memset((unsigned char *)ctx->fb_base + offset, 0, length);
    }
    pthread_mutex_unlock(&ctx->fb_mtx);
}

/* 尝试把 visible framebuffer 扩展成两页；失败只关闭 VSYNC 切页能力，不影响旧路径。 */
static int display_try_enable_pan(ipcam_display_ctx_t *ctx,
                                  struct fb_var_screeninfo *vinfo,
                                  struct fb_fix_screeninfo *finfo)
{
    if (!ctx || !vinfo || !finfo || vinfo->yres == 0) return 0;
    uint64_t wanted_virtual_h = (uint64_t)vinfo->yres * 2ULL;
    if (wanted_virtual_h > UINT32_MAX) return 0;

    struct fb_var_screeninfo next_var = *vinfo;
    struct fb_fix_screeninfo next_fix = *finfo;
    if (next_var.yres_virtual < (uint32_t)wanted_virtual_h) {
        next_var.yres_virtual = (uint32_t)wanted_virtual_h;
        next_var.xoffset = 0;
        next_var.yoffset = 0;
        next_var.activate = FB_ACTIVATE_NOW;
        if (ioctl(ctx->fb_fd, FBIOPUT_VSCREENINFO, &next_var) < 0) {
            MLOGW("fb double buffer unavailable: FBIOPUT_VSCREENINFO: %s\n",
                  strerror(errno));
            return 0;
        }
    }
    if (ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &next_var) < 0 ||
        ioctl(ctx->fb_fd, FBIOGET_FSCREENINFO, &next_fix) < 0) {
        MLOGW("fb double buffer unavailable: reread geometry failed: %s\n",
              strerror(errno));
        return 0;
    }

    /* LVGL 的两个页指针都从 virtual buffer 起点计算；如果 bootloader 留下
     * 了 page1 作为当前页，先切回 page0，避免首帧坐标整体偏移一屏。 */
    if (next_var.xoffset != 0 || next_var.yoffset != 0) {
        next_var.xoffset = 0;
        next_var.yoffset = 0;
        if (ioctl(ctx->fb_fd, FBIOPAN_DISPLAY, &next_var) < 0 ||
            ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &next_var) < 0) {
            MLOGW("fb double buffer unavailable: reset visible page failed: %s\n",
                  strerror(errno));
            return 0;
        }
    }

    uint64_t page_size = (uint64_t)next_fix.line_length * next_var.yres;
    if (next_var.yres_virtual < next_var.yres * 2U ||
        next_fix.ypanstep == 0 || page_size == 0 || page_size > SIZE_MAX / 2U ||
        page_size * 2U > next_fix.smem_len) {
        MLOGW("fb double buffer unavailable: virtual=%ux%u ypanstep=%u "
              "page=%llu smem=%u\n", next_var.xres_virtual, next_var.yres_virtual,
              next_fix.ypanstep, (unsigned long long)page_size, next_fix.smem_len);
        return 0;
    }
    *vinfo = next_var;
    *finfo = next_fix;
    ctx->fb_pan_enabled = 1;
    ctx->fb_yres_virtual = (int)next_var.yres_virtual;
    ctx->fb_page_size = (size_t)page_size;
    ctx->fb_active_page = 0;
    MLOGI("fb double buffer ready: virtual=%ux%u page=%zu ypanstep=%u "
          "present=FBIOPAN_DISPLAY(VSYNC)\n", next_var.xres, next_var.yres_virtual,
          ctx->fb_page_size, next_fix.ypanstep);
    return 1;
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

    if (vinfo.bits_per_pixel != 16) {
        MLOGE("fb bpp=%u not supported (only 16)\n", vinfo.bits_per_pixel);
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        return -1;
    }
    /* line_length 通常 = xres*2；如果硬件有 padding，需 ≥ xres*2 */
    if (finfo.line_length < vinfo.xres * 2U) {
        MLOGE("fb line_length=%u too small for w=%u bpp=16\n",
              finfo.line_length, vinfo.xres);
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        return -1;
    }
    /* mxsfb 支持 yres_virtual=2*yres 和 ypanstep=1；启用后 LVGL 只切页，
     * 不再逐行覆盖 LCD 当前扫描的那一页，从根源上消除由扫描竞争造成的撕裂。 */
    (void)display_try_enable_pan(ctx, &vinfo, &finfo);

    ctx->fb_w  = (int)vinfo.xres;
    ctx->fb_h  = (int)vinfo.yres;
    ctx->fb_bpp = (int)vinfo.bits_per_pixel;
    ctx->fb_xoffset = (int)vinfo.xoffset;
    ctx->fb_yoffset = (int)vinfo.yoffset;
    ctx->fb_yres_virtual = (int)vinfo.yres_virtual;
    ctx->fb_line_length = (int)finfo.line_length;
    ctx->fb_size = finfo.smem_len;
    if (!ctx->fb_page_size)
        ctx->fb_page_size = (size_t)ctx->fb_line_length * (size_t)ctx->fb_h;

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

    display_probe_backlight(ctx);
    /* 只清理可见页和备用页，不能把 32 MiB 的驱动映射区全部写零。 */
    display_clear_framebuffer(ctx);
    MLOGI("fb ready: visible=%dx%d virtual=%dx%d bpp=%d line_length=%d "
          "page=%zu pan=%d size=%zu\n", ctx->fb_w, ctx->fb_h,
          vinfo.xres_virtual, vinfo.yres_virtual, ctx->fb_bpp,
          ctx->fb_line_length, ctx->fb_page_size, ctx->fb_pan_enabled, ctx->fb_size);
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
        int target_w;
        int target_h;
        int target_stride;
        uint64_t target_generation;
        float zoom, center_x, center_y;
        pthread_mutex_lock(&ctx->view_mtx);
        view_flag = ctx->view_enabled;
        paused_flag = ctx->screen_paused;
        target_w = ctx->preview_w;
        target_h = ctx->preview_h;
        target_stride = ctx->preview_stride_pixels;
        target_generation = ctx->preview_target_generation;
        enabled = view_flag && !paused_flag &&
                  target_w > 0 && target_h > 0 &&
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
                display_clear_framebuffer(ctx);
            pthread_mutex_lock(&ctx->preview_mtx);
            ctx->preview_valid = 0;
            pthread_mutex_unlock(&ctx->preview_mtx);
            last_enabled = 0;
            usleep(50 * 1000);
            continue;
        }
        if (last_enabled != 1)
            MLOGI("preview active: target=%dx%d zoom=%.2f center=%.3f,%.3f "
                  "writer=%d\n", target_w, target_h, zoom, center_x, center_y,
                  ctx->framebuffer_writer_enabled);
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
        float dst_ratio = (float)target_w /
                          (float)(target_h > 0 ? target_h : 1);
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
        /* 页面切换可能在本次转换期间改变目标尺寸；发布前重新核对代数，
         * 防止旧尺寸帧被 LVGL 当成新页面的图像源。 */
        pthread_mutex_lock(&ctx->view_mtx);
        int target_stale = target_generation != ctx->preview_target_generation;
        pthread_mutex_unlock(&ctx->view_mtx);
        if (target_stale) continue;

        /* 直接转换到当前视频窗口大小；LVGL 只需复制同尺寸 RGB565，不再执行
         * 800×480 全屏转换、全帧复制和 image stretch，降低 i.MX6ULL CPU 负担。 */
        pthread_mutex_lock(&ctx->preview_mtx);
        yuyv_to_rgb565_scaled(frame.rawData, src_w, src_h,
                              ctx->preview_base, target_w, target_h, target_stride,
                              src_stride,
                              crop_x, crop_y, crop_w, crop_h,
                              ipcam_param_get_mirror_horizontal(),
                              ipcam_param_get_mirror_vertical());
        if (ctx->framebuffer_writer_enabled &&
            target_w == ctx->out_w && target_h == ctx->out_h) {
            pthread_mutex_lock(&ctx->fb_mtx);
            for (int y = 0; y < target_h; y++) {
                memcpy((unsigned char *)ctx->fb_base +
                           (size_t)(y + ctx->fb_yoffset) * ctx->fb_line_length +
                           (size_t)ctx->fb_xoffset * sizeof(*ctx->preview_base),
                       ctx->preview_base + (size_t)y * target_stride,
                       (size_t)target_w * sizeof(*ctx->preview_base));
            }
            pthread_mutex_unlock(&ctx->fb_mtx);
        }
        ctx->preview_frame = frame;
        ctx->preview_frame.rawData = ctx->preview_base;
        ctx->preview_frame.size = ctx->preview_size;
        ctx->preview_frame.width = (uint16_t)target_w;
        ctx->preview_frame.height = (uint16_t)target_h;
        ctx->preview_frame.stride = (uint32_t)target_stride * 2U;
        ctx->preview_frame.pixel_format = IPCAM_PIXEL_FORMAT_RGB565;
        ctx->preview_valid = 1;
        pthread_mutex_unlock(&ctx->preview_mtx);
        pthread_mutex_lock(&ctx->stats_mtx);
        ctx->frames_rendered++;
        pthread_mutex_unlock(&ctx->stats_mtx);
        frames++;
        /* 与 BCF2 视频线程一致，首批帧和周期帧带上源序号/时间戳，便于把 LCD
         * 卡顿与 CSI、颜色转换或 framebuffer 提交区分开。 */
        if (frames <= 3 || (frames % 30) == 0) {
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
    (void)pthread_once(&s_yuyv_lut_once, yuyv_init_lut);
    ctx->fb_fd = -1;
    ctx->fb_base = NULL;
    ctx->rb = rb;
    ctx->running = running;
    ctx->service_running = 1;
    ctx->framebuffer_writer_enabled = framebuffer_writer ? 1 : 0;
    ctx->src_w = src_w > 0 ? src_w : IPCAM_CAPTURE_WIDTH;
    ctx->src_h = src_h > 0 ? src_h : IPCAM_CAPTURE_HEIGHT;
    pthread_mutex_init(&ctx->fb_mtx, NULL);
    pthread_mutex_init(&ctx->view_mtx, NULL);
    pthread_mutex_init(&ctx->preview_mtx, NULL);
    pthread_mutex_init(&ctx->stats_mtx, NULL);
    ctx->view_enabled = ipcam_param_get_preview_enabled() ? 1 : 0;
    ctx->zoom = 1.0f;
    ctx->center_x = 0.5f;
    ctx->center_y = 0.5f;

    if (display_open_fb(ctx) < 0) {
        pthread_mutex_destroy(&ctx->fb_mtx);
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
        pthread_mutex_destroy(&ctx->fb_mtx);
        pthread_mutex_destroy(&ctx->stats_mtx);
        pthread_mutex_destroy(&ctx->preview_mtx);
        pthread_mutex_destroy(&ctx->view_mtx);
        return -1;
    }
    ctx->preview_capacity = (size_t)ctx->out_w * (size_t)ctx->out_h *
                            sizeof(*ctx->preview_base);
    ctx->preview_w = ctx->out_w;
    ctx->preview_h = ctx->out_h;
    ctx->preview_stride_pixels = ctx->out_w;
    ctx->preview_size = ctx->preview_capacity;
    ctx->preview_base = calloc(1, ctx->preview_capacity);
    if (!ctx->preview_base) {
        MLOGE("alloc preview RGB565 buffer failed (%zu bytes)\n", ctx->preview_capacity);
        munmap(ctx->fb_base, ctx->fb_size);
        ctx->fb_base = NULL;
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        pthread_mutex_destroy(&ctx->fb_mtx);
        pthread_mutex_destroy(&ctx->stats_mtx);
        pthread_mutex_destroy(&ctx->preview_mtx);
        pthread_mutex_destroy(&ctx->view_mtx);
        return -1;
    }
    /* 背光已经在 display_open_fb 中自动探测；失败不能阻塞视频服务启动。 */
    if (ipcam_display_set_backlight(ctx, ipcam_param_get_backlight_percent()) != 0)
        MLOGW("backlight initial state unavailable; sleep will use best-effort fb blank\n");

    s_default_display = ctx;

    if (pthread_create(&ctx->thread, NULL, display_thread, ctx) != 0) {
        MLOGE("pthread_create display failed\n");
        if (ctx->fb_base) munmap(ctx->fb_base, ctx->fb_size);
        if (ctx->fb_fd >= 0) close(ctx->fb_fd);
        free(ctx->preview_base);
        ctx->preview_base = NULL;
        s_default_display = NULL;
        pthread_mutex_destroy(&ctx->fb_mtx);
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
    ctx->preview_capacity = 0;
    if (s_default_display == ctx) s_default_display = NULL;
    pthread_mutex_destroy(&ctx->fb_mtx);
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
    if (enabled)
        (void)ipcam_display_set_preview_target(ctx, ctx->out_w, ctx->out_h);
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

/*
 * 设置当前页面的视频输出尺寸。
 * display 线程读取同一把 view_mtx 的尺寸和代数，页面切换时先使旧帧失效，
 * 从而不会把 P01 的 476×268 数据误交给 P03 或非视频页面。
 */
int ipcam_display_set_preview_target(ipcam_display_ctx_t *ctx,
                                     int width, int height)
{
    if (!ctx || width < 0 || height < 0 ||
        (width == 0) != (height == 0) || width > ctx->out_w || height > ctx->out_h)
        return -1;
    if (width > 0 && (size_t)width > SIZE_MAX / (size_t)height / sizeof(uint16_t))
        return -1;
    size_t size = (size_t)width * (size_t)height * sizeof(uint16_t);
    /* 先校验容量，再修改 view_mtx 下的目标；失败时不能留下一个无法生产的尺寸。 */
    if (size > ctx->preview_capacity) return -1;
    int changed;
    uint64_t generation;
    pthread_mutex_lock(&ctx->view_mtx);
    changed = ctx->preview_w != width || ctx->preview_h != height;
    if (changed) {
        ctx->preview_w = width;
        ctx->preview_h = height;
        ctx->preview_stride_pixels = width;
        ctx->preview_target_generation++;
    }
    generation = ctx->preview_target_generation;
    pthread_mutex_unlock(&ctx->view_mtx);
    pthread_mutex_lock(&ctx->preview_mtx);
    ctx->preview_size = size;
    ctx->preview_valid = 0;
    pthread_mutex_unlock(&ctx->preview_mtx);
    if (changed)
        MLOGI("preview target changed: %dx%d generation=%llu\n", width, height,
              (unsigned long long)generation);
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

/* 通过已探测的 sysfs 节点设置背光；没有节点时用 0/非 0 映射 framebuffer blank。 */
int ipcam_display_set_backlight(ipcam_display_ctx_t *ctx, int percent)
{
    if (!ctx || percent < 0 || percent > 100) return -1;
    int rc = -1;
    pthread_mutex_lock(&ctx->fb_mtx);
    if (ctx->backlight_available) {
        int value = ctx->backlight_max * percent / 100;
        FILE *fp = fopen(ctx->backlight_path, "w");
        if (fp) {
            int write_ok = fprintf(fp, "%d\n", value) > 0;
            int close_ok = fclose(fp) == 0;
            rc = write_ok && close_ok ? 0 : -1;
        }
        if (rc == 0)
            MLOGI("backlight set: percent=%d value=%d path=%s\n", percent, value,
                  ctx->backlight_path);
        else
            MLOGW("backlight write %s failed: %s\n", ctx->backlight_path,
                  strerror(errno));
    }
    if (rc != 0 && !ctx->backlight_available && ctx->fb_blank_available) {
        /* FBIOBLANK 只能表达亮/灭，非零亮度统一恢复显示，保证触摸唤醒可见。 */
        int blank = percent == 0 ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
        if (ioctl(ctx->fb_fd, FBIOBLANK, blank) == 0) {
            rc = 0;
            MLOGI("backlight fallback: FBIOBLANK=%s\n",
                  percent == 0 ? "powerdown" : "unblank");
        } else {
            MLOGW("FBIOBLANK failed: %s\n", strerror(errno));
        }
    }
    pthread_mutex_unlock(&ctx->fb_mtx);
    return rc;
}

/* 没有 sysfs 调光节点时也报告 FBIOBLANK 能力，避免屏幕设置被误判为不可用；
 * 这种回退只能实现亮/灭，具体亮度值仍按配置保存以便换回可调背光后恢复。 */
int ipcam_display_backlight_available(ipcam_display_ctx_t *ctx)
{
    return ctx ? (ctx->backlight_available || ctx->fb_blank_available) : 0;
}

/* 旧接口保留给历史调用者；主流程会把 display 上下文传入新接口。 */
int ipcam_display_set_backlight_percent(int percent)
{
    return s_default_display ? ipcam_display_set_backlight(s_default_display, percent) : -1;
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

/* 返回 framebuffer 虚拟页地址；页面由 LVGL 直接渲染，避免中间 copy。 */
void *ipcam_display_framebuffer_page(ipcam_display_ctx_t *ctx, int page)
{
    if (!ctx || !ctx->fb_base || page < 0 || page > 1 ||
        (page == 1 && !ctx->fb_pan_enabled) ||
        ctx->fb_page_size == 0 || (size_t)page > ctx->fb_size / ctx->fb_page_size)
        return NULL;
    size_t offset = (size_t)page * ctx->fb_page_size;
    if (offset >= ctx->fb_size || ctx->fb_page_size > ctx->fb_size - offset)
        return NULL;
    return (unsigned char *)ctx->fb_base + offset;
}

/*
 * 在 mxsfb 的下一次 VSYNC 切换 LVGL 已完成的 framebuffer 页面。
 * 只有最后一个 dirty area 才能提交，否则面板会在页面尚未合成完时开始扫描。
 */
int ipcam_display_present_page(ipcam_display_ctx_t *ctx, int page)
{
    if (!ctx || !ctx->fb_pan_enabled || page < 0 || page > 1) return -1;
    pthread_mutex_lock(&ctx->fb_mtx);
    struct fb_var_screeninfo vinfo;
    int rc = ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    if (rc == 0) {
        vinfo.xoffset = 0;
        vinfo.yoffset = (uint32_t)page * (uint32_t)ctx->fb_h;
        /* 仅设置 yoffset 并不保证驱动等待扫描垂直消隐；显式请求 VBL
         * 是消除全屏页面逐行撕裂的关键，mxsfb 会在下一次 VSYNC 切页。 */
        vinfo.activate = FB_ACTIVATE_VBL;
        rc = ioctl(ctx->fb_fd, FBIOPAN_DISPLAY, &vinfo);
    }
    if (rc == 0) {
        ctx->fb_xoffset = (int)vinfo.xoffset;
        ctx->fb_yoffset = (int)vinfo.yoffset;
        ctx->fb_active_page = page;
    } else {
        MLOGW("fb page present failed: page=%d errno=%d(%s)\n", page, errno,
              strerror(errno));
    }
    pthread_mutex_unlock(&ctx->fb_mtx);
    return rc;
}

/* 仅供无 pan 能力的兼容路径使用；运行时 pan 失败时不能在扫描期间整页 memcpy。 */
int ipcam_display_copy_page_to_visible(ipcam_display_ctx_t *ctx, int page)
{
    if (!ctx || !ctx->fb_base || page < 0 || page > 1 ||
        ctx->fb_page_size == 0 || (size_t)page * ctx->fb_page_size >= ctx->fb_size)
        return -1;
    pthread_mutex_lock(&ctx->fb_mtx);
    size_t src_offset = (size_t)page * ctx->fb_page_size;
    size_t dst_offset = (size_t)ctx->fb_yoffset * (size_t)ctx->fb_line_length;
    int rc = -1;
    if (dst_offset < ctx->fb_size && ctx->fb_page_size <= ctx->fb_size - dst_offset) {
        if (src_offset != dst_offset)
            memcpy((unsigned char *)ctx->fb_base + dst_offset,
                   (unsigned char *)ctx->fb_base + src_offset, ctx->fb_page_size);
        rc = 0;
    }
    pthread_mutex_unlock(&ctx->fb_mtx);
    return rc;
}

/* 单缓冲 LVGL 回退路径按目标区域逐行写入，处理 line_length 和当前 yoffset。 */
int ipcam_display_blit_area(ipcam_display_ctx_t *ctx, int x1, int y1,
                            int x2, int y2, const uint8_t *color_p)
{
    if (!ctx || !ctx->fb_base || !color_p || x1 < 0 || y1 < 0 ||
        x2 < x1 || y2 < y1 || x2 >= ctx->out_w || y2 >= ctx->out_h)
        return -1;
    size_t width_bytes = (size_t)(x2 - x1 + 1) * sizeof(uint16_t);
    size_t height = (size_t)(y2 - y1 + 1);
    if (height > SIZE_MAX / width_bytes) return -1;
    pthread_mutex_lock(&ctx->fb_mtx);
    for (int y = y1; y <= y2; y++) {
        unsigned char *target = (unsigned char *)ctx->fb_base +
                                (size_t)(y + ctx->fb_yoffset) *
                                    (size_t)ctx->fb_line_length +
                                (size_t)(x1 + ctx->fb_xoffset) * sizeof(uint16_t);
        memcpy(target, color_p + (size_t)(y - y1) * width_bytes, width_bytes);
    }
    pthread_mutex_unlock(&ctx->fb_mtx);
    (void)height;
    return 0;
}
