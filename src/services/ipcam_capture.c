#define _GNU_SOURCE
#include "ipcam_capture.h"
#include "ipcam_frame_diag.h"
#include "ipcam_log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <limits.h>
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

#define V4L2_BUFS  4
#define IPCAM_VIDEO_SYSFS_DIR "/sys/class/video4linux"
#define IPCAM_VIDEO_NAME_MAX  64
#define IPCAM_MAX_VIDEO_NODES 64
#define IPCAM_CAPTURE_LOG_MODULE "CAP "

static int xioctl(int fd, int req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static uint64_t capture_monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t capture_timeval_us(const struct timeval *tv)
{
    if (!tv || tv->tv_sec < 0 || tv->tv_usec < 0) return 0;
    return (uint64_t)tv->tv_sec * 1000000ULL +
           (uint64_t)tv->tv_usec;
}

/*
 * 从当前 packed 4:2:2 帧均匀抽取 8x8 个 Y 样本，作为自动补光的亮度输入。
 * 这里不使用整帧平均，避免在单核 i.MX6ULL 上为了点灯控制额外扫完整帧。
 */
static unsigned int capture_luma_mean(const unsigned char *src, int width,
                                      int height, size_t bytesperline)
{
    unsigned long sum = 0;
    unsigned int samples = 0;

    if (!src || width < 2 || height < 1 ||
        bytesperline < (size_t)width * 2U)
        return 0;

    for (int gy = 0; gy < 8; gy++) {
        int y = height == 1 ? 0 : gy * (height - 1) / 7;
        const unsigned char *row = src + (size_t)y * bytesperline;
        for (int gx = 0; gx < 8; gx++) {
            int x = width == 2 ? 0 : gx * (width - 2) / 7;
            x &= ~1;
#if IPCAM_CAP_PIXFMT == 1
            sum += row[(size_t)x * 2U + 1U]; /* UYVY: Cb Y0 Cr Y1 */
#else
            sum += row[(size_t)x * 2U];      /* YUYV: Y0 Cb Y1 Cr */
#endif
            samples++;
        }
    }
    return samples ? (unsigned int)(sum / samples) : 0;
}

static void capture_teardown(ipcam_capture_ctx_t *ctx)
{
    /*
     * 先停止驱动对 MMAP buffer 的使用，再解除映射；这样清理顺序与
     * V4L2 的 buffer 所有权一致，避免驱动仍在 DMA 时用户态回收内存。
     */
    if (ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }

    if (ctx->bufs) {
        for (int i = 0; i < ctx->n_bufs; i++) {
            if (ctx->bufs[i].start && ctx->bufs[i].start != MAP_FAILED)
                munmap(ctx->bufs[i].start, ctx->bufs[i].length);
        }
        free(ctx->bufs);
        ctx->bufs = NULL;
        ctx->n_bufs = 0;
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

typedef struct {
    int  number;
    char path[IPCAM_CAPTURE_DEVICE_PATH_MAX];
} capture_video_node_t;

static void capture_copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    /* 用长度明确的 memcpy 复制固定上限，避免 strncpy 的“可能不补 NUL”语义。 */
    len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* 只接受 /dev/video 后面完整的十进制编号，避免把其它设备名误当候选节点。 */
static int capture_video_number(const char *name)
{
    char *end = NULL;
    unsigned long number;

    if (!name || strncmp(name, "video", 5) != 0 ||
        !isdigit((unsigned char)name[5])) {
        return -1;
    }

    errno = 0;
    number = strtoul(name + 5, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || number > 0x7fffffffUL)
        return -1;
    return (int)number;
}

static int capture_video_node_compare(const void *lhs, const void *rhs)
{
    const capture_video_node_t *a = lhs;
    const capture_video_node_t *b = rhs;
    return a->number < b->number ? -1 : a->number > b->number;
}

static int capture_collect_video_nodes(capture_video_node_t *nodes, size_t capacity)
{
    DIR *dir;
    struct dirent *entry;
    size_t count = 0;

    if (!nodes || capacity == 0) return -1;

    dir = opendir("/dev");
    if (!dir) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE, "opendir /dev failed: %s\n", strerror(errno));
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        int number = capture_video_number(entry->d_name);
        if (number < 0) continue;
        if (count >= capacity) {
            MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                    "too many /dev/videoN nodes; ignoring %s\n", entry->d_name);
            continue;
        }

        nodes[count].number = number;
        snprintf(nodes[count].path, sizeof(nodes[count].path),
                 "/dev/video%d", number);
        count++;
    }
    closedir(dir);

    qsort(nodes, count, sizeof(nodes[0]), capture_video_node_compare);
    return (int)count;
}

static void capture_read_sysfs_name(const char *device_path,
                                    char *name, size_t name_size)
{
    const char *base;
    char sysfs_path[IPCAM_CAPTURE_DEVICE_PATH_MAX + 64];
    FILE *fp;

    if (!name || name_size == 0) return;
    name[0] = '\0';

    base = strrchr(device_path ? device_path : "", '/');
    if (!base || !base[1]) return;
    int n = snprintf(sysfs_path, sizeof(sysfs_path), "%s/%s/name",
                     IPCAM_VIDEO_SYSFS_DIR, base + 1);
    if (n < 0 || n >= (int)sizeof(sysfs_path)) {
        return;
    }

    fp = fopen(sysfs_path, "r");
    if (!fp) return;
    if (fgets(name, (int)name_size, fp)) {
        name[strcspn(name, "\r\n")] = '\0';
    } else {
        name[0] = '\0';
    }
    fclose(fp);
}

/*
 * 把名称中的大小写、点号、短横线和下划线归一化后匹配 mx6s-csi。
 * 官方驱动的 sysfs 名称通常是 mx6s-csi，但 QUERYCAP 的 card 可能写成
 * i.MX6S_CSI；统一比较可兼容这两种同一驱动的命名方式。
 */
static int capture_text_matches_csi(const char *text)
{
    char normalized[IPCAM_VIDEO_NAME_MAX];
    size_t i, out = 0;

    if (!text) return 0;
    for (i = 0; text[i] && out + 1 < sizeof(normalized); i++) {
        unsigned char c = (unsigned char)text[i];
        if (isalnum(c)) normalized[out++] = (char)tolower(c);
    }
    normalized[out] = '\0';
    return strstr(normalized, "mx6scsi") != NULL;
}

static int capture_querycap(int fd, struct v4l2_capability *cap,
                            __u32 *device_caps)
{
    if (!cap || !device_caps) return -1;
    memset(cap, 0, sizeof(*cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, cap) < 0) return -1;

    /* 内核返回的固定长度字段理论上带 NUL，这里再兜底避免日志越界。 */
    cap->driver[sizeof(cap->driver) - 1] = '\0';
    cap->card[sizeof(cap->card) - 1] = '\0';
    cap->bus_info[sizeof(cap->bus_info) - 1] = '\0';

    *device_caps = cap->capabilities;
    if (*device_caps & V4L2_CAP_DEVICE_CAPS)
        *device_caps = cap->device_caps;
    return 0;
}

static int capture_has_required_caps(__u32 device_caps)
{
    return (device_caps & V4L2_CAP_VIDEO_CAPTURE) &&
           (device_caps & V4L2_CAP_STREAMING);
}

/*
 * 将 V4L2 FourCC 转成固定长度文本并保留十六进制日志的可读性。
 * 旧版 mx6s-csi 的 G_FMT 会把 pixelformat 留为 0；直接使用 %.4s 时日志
 * 会显示为空，现场无法区分“驱动返回 0”与“应用没有打印”。
 */
static void capture_fourcc_text(__u32 fourcc, char text[5])
{
    if (!text) return;
    if (fourcc == 0) {
        memcpy(text, "NONE", 5);
        return;
    }

    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)((fourcc >> (i * 8)) & 0xff);
        text[i] = isprint(c) ? (char)c : '?';
    }
    text[4] = '\0';
}

static int capture_is_csi_device(const char *sysfs_name,
                                 const struct v4l2_capability *cap)
{
    return capture_text_matches_csi(sysfs_name) ||
           (cap && (capture_text_matches_csi((const char *)cap->driver) ||
                    capture_text_matches_csi((const char *)cap->card)));
}

static void capture_log_candidate(const char *path, const char *sysfs_name,
                                  const struct v4l2_capability *cap,
                                  __u32 device_caps)
{
    MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
          "video candidate %s: name=%s driver=%s card=%s caps=0x%08x\n",
          path,
          (sysfs_name && sysfs_name[0]) ? sysfs_name : "unknown",
          cap ? (const char *)cap->driver : "unknown",
          cap ? (const char *)cap->card : "unknown",
          device_caps);
}

/*
 * 解析 S_FMT/G_FMT；packed 4:2:2 字节序由 IPCAM_CAP_PIXFMT 编译期选定。
 * 为什么放在编译期而不是运行时参数：像素格式决定整条链路（display/encode）
 * 的字节序假设，运行中切换没有业务意义；编译期选择还能让两个消费者
 * 的字节序分支直接在编译时定死，无运行时开销。
 */
static int capture_negotiate_pixfmt(ipcam_capture_ctx_t *ctx)
{
    struct v4l2_format fmt;
    __u32 s_fmt_pixelformat;
    __u32 g_fmt_pixelformat;
    __u32 s_fmt_bytesperline;
    __u32 s_fmt_sizeimage;
    __u32 effective_bytesperline;
    __u32 effective_sizeimage;
    int requested_width = ctx->width;
    int requested_height = ctx->height;
    size_t expected_bytesperline = (size_t)requested_width * 2;
    size_t expected_sizeimage = expected_bytesperline * requested_height;
#if IPCAM_CAP_PIXFMT == 1
    __u32 want = V4L2_PIX_FMT_UYVY;
#else
    __u32 want = V4L2_PIX_FMT_YUYV;
#endif
    char want_text[5];
    char s_fmt_text[5];
    char g_fmt_text[5];

    capture_fourcc_text(want, want_text);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = ctx->width;
    fmt.fmt.pix.height = ctx->height;
    fmt.fmt.pix.pixelformat = want;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE, "S_FMT %s(0x%08x) failed: %s\n",
              want_text, want, strerror(errno));
        return -1;
    }

    /* S_FMT 的回写结果是驱动对请求格式的第一次确认。 */
    s_fmt_pixelformat = fmt.fmt.pix.pixelformat;
    capture_fourcc_text(s_fmt_pixelformat, s_fmt_text);
    if (s_fmt_pixelformat != want) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s S_FMT returned %s(0x%08x), expected %s(0x%08x)\n",
              ctx->device_path, s_fmt_text, s_fmt_pixelformat,
              want_text, want);
        return -1;
    }
    if ((int)fmt.fmt.pix.width != requested_width ||
        (int)fmt.fmt.pix.height != requested_height) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s adjusted S_FMT size from %dx%d to %ux%u; refusing\n",
              ctx->device_path, requested_width, requested_height,
              fmt.fmt.pix.width, fmt.fmt.pix.height);
        return -1;
    }

    s_fmt_bytesperline = fmt.fmt.pix.bytesperline;
    s_fmt_sizeimage = fmt.fmt.pix.sizeimage;
    if (s_fmt_bytesperline != expected_bytesperline ||
        s_fmt_sizeimage != expected_sizeimage) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s returned invalid S_FMT layout: bytesperline=%u sizeimage=%u, "
              "expected %zu/%zu\n",
              ctx->device_path, s_fmt_bytesperline, s_fmt_sizeimage,
              expected_bytesperline, expected_sizeimage);
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_G_FMT, &fmt) < 0) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE, "G_FMT: %s\n", strerror(errno));
        return -1;
    }

    g_fmt_pixelformat = fmt.fmt.pix.pixelformat;
    capture_fourcc_text(g_fmt_pixelformat, g_fmt_text);
    if (g_fmt_pixelformat != want &&
        !(g_fmt_pixelformat == 0 &&
          ctx->legacy_gfmt_pixelformat_missing &&
          s_fmt_pixelformat == want)) {
        /* 明确打印期望/实际 FourCC，避免消费者按错误字节序解释图像。 */
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s did not honor %s(0x%08x) (G_FMT=%s(0x%08x))\n",
              ctx->device_path, want_text, want,
              g_fmt_text, g_fmt_pixelformat);
        return -1;
    }

    if (g_fmt_pixelformat == 0) {
        /*
         * 这是出厂 4.1.15 mx6s_capture 的已知兼容行为，不代表 CSI 没有
         * 配置 YUYV：S_FMT 已回显 YUYV，驱动内部也按该格式设置 csi_dev->fmt。
         * 其它驱动仍必须由 G_FMT 返回非零且匹配的 FourCC。
         */
        MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s G_FMT returned pixelformat=NONE; using S_FMT=%s for legacy mx6s-csi\n",
              ctx->device_path, s_fmt_text);
    }

    /* 环形缓冲在打开设备前已按请求尺寸分配，不能接受驱动静默改尺寸。 */
    if ((int)fmt.fmt.pix.width != requested_width ||
        (int)fmt.fmt.pix.height != requested_height) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s adjusted capture size from %dx%d to %ux%u; refusing\n",
              ctx->device_path, requested_width, requested_height,
              fmt.fmt.pix.width, fmt.fmt.pix.height);
        return -1;
    }

    /*
     * 出厂 4.1.15 mx6s-csi 的 G_FMT 会复制 csi_dev->pix，但该驱动在 S_FMT
     * 中没有回填 pix.bytesperline，故 G_FMT 的行跨度可能为 0。已确认是 CSI
     * 节点时使用 S_FMT 已确认的布局；其它设备仍必须完整回显布局，防止把
     * 带 padding 的帧按紧凑帧解释而产生逐行错位。
     */
    effective_bytesperline = fmt.fmt.pix.bytesperline;
    effective_sizeimage = fmt.fmt.pix.sizeimage;
    if (effective_bytesperline == 0 &&
        ctx->legacy_gfmt_pixelformat_missing) {
        effective_bytesperline = s_fmt_bytesperline;
        MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s G_FMT returned bytesperline=0; using S_FMT bytesperline=%u "
              "for legacy mx6s-csi\n",
              ctx->device_path, effective_bytesperline);
    }
    if (effective_sizeimage == 0 &&
        ctx->legacy_gfmt_pixelformat_missing) {
        effective_sizeimage = s_fmt_sizeimage;
        MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s G_FMT returned sizeimage=0; using S_FMT sizeimage=%u "
              "for legacy mx6s-csi\n",
              ctx->device_path, effective_sizeimage);
    }

    if (effective_bytesperline != expected_bytesperline ||
        effective_sizeimage != expected_sizeimage) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
              "%s returned invalid G_FMT layout: bytesperline=%u sizeimage=%u, "
              "expected %zu/%zu; refusing\n",
              ctx->device_path, effective_bytesperline, effective_sizeimage,
              expected_bytesperline, expected_sizeimage);
        return -1;
    }

    ctx->width  = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;
    ctx->bytesperline = effective_bytesperline;
    ctx->frame_bytes = effective_sizeimage;
    MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
          "camera negotiated: %s %dx%d fmt=%s bytesperline=%zu sizeimage=%zu\n",
          ctx->device_path, ctx->width, ctx->height,
          g_fmt_pixelformat == 0 ? s_fmt_text : g_fmt_text,
          ctx->bytesperline, ctx->frame_bytes);
    return 0;
}

static int capture_open_device(ipcam_capture_ctx_t *ctx)
{
    struct v4l2_capability selected_cap;
    __u32 selected_caps = 0;
    char selected_name[IPCAM_VIDEO_NAME_MAX] = { 0 };

    if (IPCAM_VIDEO_DEV[0] != '\0') {
        struct v4l2_capability cap;
        char sysfs_name[IPCAM_VIDEO_NAME_MAX] = { 0 };
        __u32 device_caps = 0;
        int fd = open(IPCAM_VIDEO_DEV, O_RDWR);

        if (fd < 0) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE, "open configured camera %s: %s\n",
                  IPCAM_VIDEO_DEV, strerror(errno));
            return -1;
        }
        if (capture_querycap(fd, &cap, &device_caps) < 0) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "QUERYCAP %s failed: %s\n", IPCAM_VIDEO_DEV, strerror(errno));
            close(fd);
            return -1;
        }
        capture_read_sysfs_name(IPCAM_VIDEO_DEV, sysfs_name, sizeof(sysfs_name));
        capture_log_candidate(IPCAM_VIDEO_DEV, sysfs_name, &cap, device_caps);
        if (!capture_has_required_caps(device_caps)) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "%s lacks video-capture or streaming capability\n",
                  IPCAM_VIDEO_DEV);
            close(fd);
            return -1;
        }
        if (strlen(IPCAM_VIDEO_DEV) >= sizeof(ctx->device_path)) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "configured camera path is too long: %s\n", IPCAM_VIDEO_DEV);
            close(fd);
            return -1;
        }

        ctx->fd = fd;
        strcpy(ctx->device_path, IPCAM_VIDEO_DEV);
        ctx->legacy_gfmt_pixelformat_missing =
            capture_is_csi_device(sysfs_name, &cap);
        MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
              "camera device selected by override: %s (name=%s driver=%s card=%s)\n",
              ctx->device_path,
              sysfs_name[0] ? sysfs_name : "unknown",
              (const char *)cap.driver, (const char *)cap.card);
    } else {
        capture_video_node_t nodes[IPCAM_MAX_VIDEO_NODES];
        int node_count = capture_collect_video_nodes(
            nodes, sizeof(nodes) / sizeof(nodes[0]));
        int selected_fd = -1;

        if (node_count < 0) return -1;
        if (node_count == 0) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "no /dev/videoN nodes found; CSI camera is unavailable\n");
            return -1;
        }

        /*
         * 不按 video 编号或“第一个有 capture capability 的节点”猜测：
         * PxP/USB 等其它 V4L2 设备可能同时存在，自动模式只接受 mx6s-csi。
         * 遍历完所有候选后才决定结果，失败日志能完整反映板端现场。
         */
        for (int i = 0; i < node_count; i++) {
            struct v4l2_capability cap;
            char sysfs_name[IPCAM_VIDEO_NAME_MAX] = { 0 };
            __u32 device_caps = 0;
            int fd = open(nodes[i].path, O_RDWR);

            if (fd < 0) {
                MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                      "open video candidate %s failed: %s\n",
                      nodes[i].path, strerror(errno));
                continue;
            }
            if (capture_querycap(fd, &cap, &device_caps) < 0) {
                MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                      "QUERYCAP %s failed: %s\n",
                      nodes[i].path, strerror(errno));
                close(fd);
                continue;
            }

            capture_read_sysfs_name(nodes[i].path, sysfs_name, sizeof(sysfs_name));
            capture_log_candidate(nodes[i].path, sysfs_name, &cap, device_caps);
            if (!capture_has_required_caps(device_caps)) {
                MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
                      "skip %s: not a streaming video-capture node\n",
                      nodes[i].path);
                close(fd);
                continue;
            }
            if (!capture_is_csi_device(sysfs_name, &cap)) {
                MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
                      "skip %s: candidate is not mx6s-csi\n", nodes[i].path);
                close(fd);
                continue;
            }

            if (selected_fd >= 0) {
                MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                      "multiple mx6s-csi nodes found; keeping the lowest-numbered one\n");
                close(fd);
                continue;
            }

            selected_fd = fd;
            selected_cap = cap;
            selected_caps = device_caps;
            capture_copy_text(selected_name, sizeof(selected_name), sysfs_name);
            capture_copy_text(ctx->device_path, sizeof(ctx->device_path),
                              nodes[i].path);
        }

        if (selected_fd < 0) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "auto discovery found no mx6s-csi capture node; refusing non-CSI video devices\n");
            return -1;
        }

        ctx->fd = selected_fd;
        ctx->legacy_gfmt_pixelformat_missing = 1;
        MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
              "camera device selected automatically: %s (name=%s driver=%s card=%s caps=0x%08x)\n",
              ctx->device_path,
              selected_name[0] ? selected_name : "unknown",
              (const char *)selected_cap.driver,
              (const char *)selected_cap.card, selected_caps);
    }

    if (capture_negotiate_pixfmt(ctx) < 0) {
        capture_teardown(ctx);
        return -1;
    }
    return 0;
}

static int capture_init_mmap(ipcam_capture_ctx_t *ctx)
{
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    memset(&req, 0, sizeof(req));
    req.count = V4L2_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE, "REQBUFS: %s\n", strerror(errno));
        return -1;
    }
    if (req.count < 2) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
              "insufficient buffer memory (got %u)\n", req.count);
        return -1;
    }
    ctx->n_bufs = req.count;
    ctx->bufs = calloc((size_t)ctx->n_bufs, sizeof(*ctx->bufs));
    if (!ctx->bufs) {
        ctx->n_bufs = 0;
        return -1;
    }

    for (int i = 0; i < ctx->n_bufs; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "QUERYBUF %d: %s\n", i, strerror(errno));
            capture_teardown(ctx);
            return -1;
        }

        ctx->bufs[i].length = buf.length;
        if (ctx->frame_bytes == 0 || ctx->frame_bytes > buf.length) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "QUERYBUF %d length=%u is smaller than frame_bytes=%zu\n",
                  i, buf.length, ctx->frame_bytes);
            capture_teardown(ctx);
            return -1;
        }
        ctx->bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, ctx->fd, buf.m.offset);
        if (ctx->bufs[i].start == MAP_FAILED) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "mmap buf %d: %s\n", i, strerror(errno));
            ctx->bufs[i].start = NULL;
            capture_teardown(ctx);
            return -1;
        }

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "QBUF %d: %s\n", i, strerror(errno));
            capture_teardown(ctx);
            return -1;
        }
    }
    /* 启动前记录驱动实际提供的 MMAP 槽数和单槽容量，便于核对 ring 配置。 */
    MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
            "mmap ready: buffers=%d buffer_length=%zu frame_bytes=%zu\n",
            ctx->n_bufs, ctx->bufs[0].length, ctx->frame_bytes);
    return 0;
}

static void *capture_thread(void *arg)
{
    ipcam_capture_ctx_t *ctx = arg;
    struct v4l2_buffer buf;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned long dq_frames = 0, frames = 0;
    unsigned long dropped_disp = 0, dropped_enc = 0;
    unsigned long dropped_error = 0, dropped_size = 0, dropped_probe = 0;
    unsigned long sequence_gaps = 0, sequence_rewinds = 0;
    unsigned long timestamp_anomalies = 0, qbuf_errors = 0;
    unsigned long report_frames = 0;
    unsigned long long total_bytes = 0, report_bytes = 0;
    unsigned long buffer_counts[64] = { 0 };
    uint32_t last_sequence = 0;
    uint64_t last_timestamp_us = 0;
    uint64_t last_source_sequence = 0;
    uint32_t last_probe = 0;
    unsigned int last_luma = 0;
    int have_sequence = 0;
    int first_frame_logged = 0;
    struct timeval t0, t1, last_report, now;

    MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
            "capture thread start: device=%s size=%dx%d bytesperline=%zu "
            "frame_bytes=%zu\n",
            ctx->device_path, ctx->width, ctx->height,
            ctx->bytesperline, ctx->frame_bytes);

    /*
     * 白光控制与 video fd 由同一个采集线程串行访问，避免另一个线程同时向
     * OV5640 发 V4L2 control，造成旧版 sensor 驱动的 I2C 访问竞态。
     */
    ipcam_light_init(&ctx->light, ctx->fd);
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE, "STREAMON: %s\n", strerror(errno));
        ipcam_light_deinit(&ctx->light);
        if (ctx->running) *ctx->running = 0;
        return NULL;
    }

    gettimeofday(&t0, NULL);
    last_report = t0;

    while (*ctx->running) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            /* EIO = 硬件错误（CSI 接触不良 / 传感器故障），不是 EAGAIN 的可重试变体 */
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                    "DQBUF fatal: %s\n", strerror(errno));
            if (ctx->running) *ctx->running = 0;
            break;
        }

        /* 驱动返回的 index 是外部输入，必须先校验再作为数组下标使用。 */
        if (buf.index >= (unsigned int)ctx->n_bufs) {
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                  "DQBUF returned invalid index=%u (n_bufs=%d)\n",
                  buf.index, ctx->n_bufs);
            if (ctx->running) *ctx->running = 0;
            break;
        }

        dq_frames++;
        if (buf.index < sizeof(buffer_counts) / sizeof(buffer_counts[0]))
            buffer_counts[buf.index]++;

        /* sequence 应单调递增；跳号通常是驱动丢帧，回退则要重点怀疑重启/覆盖。 */
        if (have_sequence) {
            if (buf.sequence > last_sequence &&
                buf.sequence - last_sequence > 1U) {
                unsigned long gap =
                    (unsigned long)(buf.sequence - last_sequence - 1U);
                sequence_gaps += gap;
                /* 异常首次出现或累计到整百时打印上下文，避免 30 fps 刷屏。 */
                if (sequence_gaps == gap || (sequence_gaps % 100UL) < gap) {
                    MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                            "V4L2 sequence gap: previous=%u current=%u "
                            "gap=%lu total=%lu buf=%u\n",
                            last_sequence, buf.sequence, gap, sequence_gaps,
                            buf.index);
                }
            } else if (buf.sequence <= last_sequence) {
                sequence_rewinds++;
                if (sequence_rewinds == 1 || (sequence_rewinds % 100UL) == 0) {
                    MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                            "V4L2 sequence rewind: previous=%u current=%u "
                            "total=%lu buf=%u\n",
                            last_sequence, buf.sequence, sequence_rewinds,
                            buf.index);
                }
            }
        }
        last_sequence = buf.sequence;
        have_sequence = 1;

        uint64_t timestamp_us = capture_timeval_us(&buf.timestamp);
        if (last_timestamp_us != 0 && timestamp_us != 0 &&
            (timestamp_us <= last_timestamp_us ||
             timestamp_us - last_timestamp_us > 500000ULL)) {
            timestamp_anomalies++;
            if (timestamp_anomalies == 1 ||
                (timestamp_anomalies % 100UL) == 0) {
                MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                        "V4L2 timestamp anomaly: previous=%llu current=%llu "
                        "delta=%lldus sequence=%u total=%lu\n",
                        (unsigned long long)last_timestamp_us,
                        (unsigned long long)timestamp_us,
                        timestamp_us >= last_timestamp_us
                            ? (long long)(timestamp_us - last_timestamp_us)
                            : -(long long)(last_timestamp_us - timestamp_us),
                        buf.sequence, timestamp_anomalies);
            }
        }
        if (timestamp_us != 0) last_timestamp_us = timestamp_us;

        /* V4L2_BUF_FLAG_ERROR：驱动说这一帧坏了，丢弃但仍 QBUF */
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            dropped_error++;
            MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                    "frame %u marked BUF_FLAG_ERROR, dropping\n", buf.sequence);
            if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
                MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
                        "QBUF after error: %s\n", strerror(errno));
                if (ctx->running) *ctx->running = 0;
                break;
            }
            continue;
        }

        if (buf.bytesused == ctx->frame_bytes &&
            buf.bytesused <= ctx->bufs[buf.index].length) {
            const void *src = ctx->bufs[buf.index].start;
            ipcam_frame_probe_t probe;

            /*
             * 只对固定采样点做指纹，正常路径开销很小；如果这里失败，说明
             * 已经出现内部布局不一致，不能把未经诊断的帧送给两个消费者。
             */
            if (ipcam_frame_probe_pixels(src, buf.bytesused, ctx->width,
                                         ctx->height, 2, ctx->bytesperline,
                                         &probe) != 0) {
                dropped_probe++;
                if (dropped_probe == 1 || (dropped_probe % 100) == 0) {
                    MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                            "frame %u probe failed: bytesused=%u bpl=%zu "
                            "sizeimage=%zu dropping(count=%lu)\n",
                            buf.sequence, buf.bytesused, ctx->bytesperline,
                            ctx->frame_bytes, dropped_probe);
                }
            } else {
                ipcam_frame_meta_t meta;
                const char *disp_result = ctx->rb_disp ? "ok" : "off";
                const char *enc_result = ctx->rb_enc ? "ok" : "off";

                memset(&meta, 0, sizeof(meta));
                meta.source_sequence = buf.sequence;
                meta.source_buffer_index = buf.index;
                meta.source_flags = buf.flags;
                meta.source_timestamp_us = timestamp_us;
                meta.source_bytesperline = (uint32_t)ctx->bytesperline;
                meta.source_frame_bytes = (uint32_t)ctx->frame_bytes;
                meta.source_probe_global = probe.global;
                for (int q = 0; q < 4; q++)
                    meta.source_probe_quadrant[q] = probe.quadrant[q];

                last_source_sequence = meta.source_sequence;
                last_probe = probe.global;
                last_luma = capture_luma_mean(src, ctx->width, ctx->height,
                                              ctx->bytesperline);
                ipcam_light_update(&ctx->light, last_luma,
                                   capture_monotonic_us());

                /* 双路非阻塞写：任一满则丢该路（不阻塞生产者、不等消费者）。 */
                if (ctx->rb_disp) {
                    if (ipcam_ring_try_append_meta(ctx->rb_disp, src,
                                                   buf.bytesused, &meta) != 0) {
                        dropped_disp++;
                        disp_result = "drop";
                    }
                }
                if (ctx->rb_enc) {
                    if (ipcam_ring_try_append_meta(ctx->rb_enc, src,
                                                   buf.bytesused, &meta) != 0) {
                        dropped_enc++;
                        enc_result = "drop";
                    }
                }
                frames++;
                total_bytes += buf.bytesused;

                /* BCF2 的视频线程会记录首帧，首帧日志同时固定来源指纹。 */
                if (!first_frame_logged) {
                    MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
                            "first frame: v4l2_seq=%u buf=%u bytes=%u "
                            "bpl=%zu timestamp=%llu luma=%u probe=%08x "
                            "q=%08x/%08x/%08x/%08x disp=%s enc=%s\n",
                            buf.sequence, buf.index, buf.bytesused,
                            ctx->bytesperline,
                            (unsigned long long)timestamp_us, last_luma,
                            probe.global, probe.quadrant[0], probe.quadrant[1],
                            probe.quadrant[2], probe.quadrant[3],
                            disp_result, enc_result);
                    first_frame_logged = 1;
                } else if ((frames % 30UL) == 0) {
                    /* DEBUG 级别按约 1 秒抽样，打开 IPCAM_LOG_LEVEL=5 时可追踪帧内容。 */
                    MLOGD_M(IPCAM_CAPTURE_LOG_MODULE,
                            "frame probe: v4l2_seq=%u buf=%u luma=%u "
                            "probe=%08x q=%08x/%08x/%08x/%08x disp=%s enc=%s\n",
                            buf.sequence, buf.index, last_luma, probe.global,
                            probe.quadrant[0], probe.quadrant[1],
                            probe.quadrant[2], probe.quadrant[3],
                            disp_result, enc_result);
                }
            }
        } else {
            dropped_size++;
            /*
             * 摄像头持续输出异常帧时不能每帧刷串口；保留首帧和每 100 帧一次的
             * 现场信息，同时让 5 秒统计中的 drop_size 记录完整异常数量。
             */
            if (dropped_size == 1 || (dropped_size % 100) == 0) {
                MLOGW_M(IPCAM_CAPTURE_LOG_MODULE,
                        "frame %u layout invalid: buf=%u flags=0x%08x "
                        "bytesused=%u expected=%zu bytesperline=%zu "
                        "buffer_length=%zu dropping(count=%lu)\n",
                        buf.sequence, buf.index, buf.flags, buf.bytesused,
                        ctx->frame_bytes, ctx->bytesperline,
                        ctx->bufs[buf.index].length,
                        dropped_size);
            }
        }

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            qbuf_errors++;
            MLOGE_M(IPCAM_CAPTURE_LOG_MODULE, "QBUF: %s\n", strerror(errno));
            if (ctx->running) *ctx->running = 0;
            break;
        }

        /*
         * 每 5 秒打印一次累计计数和区间帧率，避免按帧打印拖慢串口；区间值
         * 能区分“驱动不出帧”和“消费者跟不上”，累计丢帧便于长期运行排查。
         */
        gettimeofday(&now, NULL);
        double report_sec = (now.tv_sec - last_report.tv_sec) +
                            (now.tv_usec - last_report.tv_usec) / 1e6;
        if (report_sec >= 5.0) {
            unsigned long interval_frames = frames - report_frames;
            unsigned long long interval_bytes = total_bytes - report_bytes;
            int disp_count = ctx->rb_disp ? ipcam_ring_count(ctx->rb_disp) : 0;
            int enc_count = ctx->rb_enc ? ipcam_ring_count(ctx->rb_enc) : 0;
            MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
                    "stats: interval=%.1fs fps=%.1f dq=%lu accepted=%lu "
                    "interval_bytes=%llu drop_disp=%lu drop_enc=%lu "
                    "drop_error=%lu drop_size=%lu drop_probe=%lu "
                    "seq_gap=%lu seq_rewind=%lu ts_anomaly=%lu qbuf_error=%lu "
                    "last={src=%llu luma=%u probe=%08x} "
                    "light={backend=%s state=%s errors=%lu} "
                    "rb_disp=%d rb_enc=%d\n",
                    report_sec,
                    report_sec > 0 ? interval_frames / report_sec : 0,
                    dq_frames, frames, interval_bytes,
                    dropped_disp, dropped_enc, dropped_error, dropped_size,
                    dropped_probe, sequence_gaps, sequence_rewinds,
                    timestamp_anomalies, qbuf_errors,
                    (unsigned long long)last_source_sequence, last_luma,
                    last_probe, ipcam_light_backend_name(&ctx->light),
                    ipcam_light_state_name(&ctx->light), ctx->light.set_errors,
                    disp_count, enc_count);
            for (int i = 0; i < ctx->n_bufs &&
                        i < (int)(sizeof(buffer_counts) / sizeof(buffer_counts[0])); i++) {
                MLOGD_M(IPCAM_CAPTURE_LOG_MODULE,
                        "v4l2 buffer[%d] dq_count=%lu\n", i, buffer_counts[i]);
            }
            last_report = now;
            report_frames = frames;
            report_bytes = total_bytes;
        }
    }

    /* 先尝试关灯再停流，兼容部分 sensor 控件只在 streaming 状态可写的 BSP。 */
    ipcam_light_force_off(&ctx->light);
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    ipcam_light_deinit(&ctx->light);

    gettimeofday(&t1, NULL);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    MLOGI_M(IPCAM_CAPTURE_LOG_MODULE,
            "capture thread exit, dq=%lu frames=%lu bytes=%llu "
            "drop_disp=%lu drop_enc=%lu drop_error=%lu drop_size=%lu "
            "drop_probe=%lu seq_gap=%lu seq_rewind=%lu ts_anomaly=%lu "
            "qbuf_error=%lu avg_fps=%.1f\n",
            dq_frames, frames, total_bytes, dropped_disp, dropped_enc,
            dropped_error, dropped_size, dropped_probe, sequence_gaps,
            sequence_rewinds, timestamp_anomalies, qbuf_errors,
            sec > 0 ? frames / sec : 0);
    return NULL;
}

int ipcam_capture_start(ipcam_capture_ctx_t *ctx,
                        ipcam_ring_buffer_t *rb_disp,
                        ipcam_ring_buffer_t *rb_enc,
                        int width,
                        int height,
                        volatile sig_atomic_t *running)
{
    if (!ctx || !running) return -1;
    if (width < 2 || width > 4096 || (width & 1) != 0 ||
        height < 1 || height > 4096) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE,
              "invalid capture dimensions %dx%d (width must be even, range 2..4096)\n",
              width, height);
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->width  = width;
    ctx->height = height;
    ctx->rb_disp = rb_disp;
    ctx->rb_enc  = rb_enc;
    ctx->running = running;

    if (capture_open_device(ctx) < 0) return -1;
    if (capture_init_mmap(ctx) < 0) {
        capture_teardown(ctx);
        return -1;
    }

    if (pthread_create(&ctx->thread, NULL, capture_thread, ctx) != 0) {
        MLOGE_M(IPCAM_CAPTURE_LOG_MODULE, "pthread_create capture failed\n");
        capture_teardown(ctx);
        return -1;
    }
    return 0;
}

void ipcam_capture_stop(ipcam_capture_ctx_t *ctx)
{
    if (!ctx) return;

    /* 1) 设 running=0，让线程退出 DQBUF 循环 */
    if (ctx->running) *ctx->running = 0;

    /*
     * 2) 只用 STREAMOFF 唤醒 DQBUF，不提前 close fd；采集线程仍可能在
     * 退出收尾阶段访问该 fd，必须等 join 完成后再由 teardown 关闭。
     */
    if (ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }

    /* 3) join（STREAMOFF 后等待线程不再访问 fd） */
    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }

    /* 4) 线程完全退出后，再统一释放 mmap 并关闭 fd。 */
    capture_teardown(ctx);
}

void ipcam_capture_get_dimensions(const ipcam_capture_ctx_t *ctx, int *w, int *h)
{
    if (!ctx) return;
    if (w) *w = ctx->width;
    if (h) *h = ctx->height;
}
