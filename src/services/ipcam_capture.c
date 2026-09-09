#define _GNU_SOURCE
/* 采集线程的正常帧、V4L2 和设备协商日志归入 CAP 模块。 */
#define IPCAM_LOG_MODULE "CAP "
#include "ipcam_capture.h"
#include "ipcam_log.h"
#include "ipcam_param.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

/* 对可被信号打断的 V4L2 ioctl 自动重试，其他错误原样返回。 */
static int xioctl(int fd, int req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

/* 统计锁只保护低频状态读取，采集线程不会因为统计消费者而阻塞媒体链路。 */
static void capture_add_stats(ipcam_capture_ctx_t *ctx, uint64_t emitted,
                              uint64_t dropped_disp, uint64_t dropped_enc)
{
    pthread_mutex_lock(&ctx->stats_mtx);
    ctx->frames_emitted += emitted;
    ctx->frames_dropped_disp += dropped_disp;
    ctx->frames_dropped_enc += dropped_enc;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/* 释放 mmap、STREAMOFF 和设备 fd；允许在部分初始化失败路径重复调用。 */
static void capture_teardown(ipcam_capture_ctx_t *ctx)
{
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
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        close(ctx->fd);
        ctx->fd = -1;
    }
}

/* 将固定长度的 V4L2 FourCC 转成可安全打印的文本；0 明确显示为 NONE。 */
static void capture_fourcc_text(uint32_t fourcc, char text[5])
{
    if (!text) return;
    if (fourcc == 0) {
        memcpy(text, "NONE", 5);
        return;
    }
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)((fourcc >> (i * 8)) & 0xffU);
        text[i] = isprint(c) ? (char)c : '?';
    }
    text[4] = '\0';
}

/*
 * 只接受完整的 /dev/videoN 名称并按编号排序，避免自动模式依赖目录返回顺序。
 * PxP、USB 等节点可能同时存在，后续还会结合 sysfs/V4L2 身份筛选 CSI。
 */
typedef struct capture_video_node_s {
    int number;
    char path[IPCAM_CAPTURE_DEVICE_PATH_MAX];
} capture_video_node_t;

static int capture_video_number(const char *name)
{
    char *end = NULL;
    unsigned long number;

    if (!name || strncmp(name, "video", 5) != 0 ||
        !isdigit((unsigned char)name[5]))
        return -1;

    errno = 0;
    number = strtoul(name + 5, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || number > INT32_MAX)
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
        MLOGE("opendir /dev failed: %s\n", strerror(errno));
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        int number = capture_video_number(entry->d_name);
        int n;
        if (number < 0) continue;
        if (count >= capacity) {
            MLOGW("too many /dev/videoN nodes; ignoring %s\n", entry->d_name);
            continue;
        }
        n = snprintf(nodes[count].path, sizeof(nodes[count].path),
                     "/dev/video%d", number);
        if (n < 0 || n >= (int)sizeof(nodes[count].path)) continue;
        nodes[count].number = number;
        count++;
    }
    closedir(dir);
    qsort(nodes, count, sizeof(nodes[0]), capture_video_node_compare);
    return (int)count;
}

/* 读取 /sys/class/video4linux/videoN/name，失败时返回空字符串而不阻塞启动。 */
static void capture_read_sysfs_name(const char *device_path,
                                    char *name, size_t name_size)
{
    const char *base;
    char sysfs_path[IPCAM_CAPTURE_DEVICE_PATH_MAX + 64];
    FILE *fp;
    int n;

    if (!name || name_size == 0) return;
    name[0] = '\0';
    base = strrchr(device_path ? device_path : "", '/');
    if (!base || !base[1]) return;
    n = snprintf(sysfs_path, sizeof(sysfs_path), "%s/%s/name",
                 IPCAM_VIDEO_SYSFS_DIR, base + 1);
    if (n < 0 || n >= (int)sizeof(sysfs_path)) return;

    fp = fopen(sysfs_path, "r");
    if (!fp) return;
    if (fgets(name, (int)name_size, fp))
        name[strcspn(name, "\r\n")] = '\0';
    fclose(fp);
}

/* 统一大小写和分隔符，兼容 sysfs 的 mx6s-csi 与 card 的 i.MX6S_CSI。 */
static int capture_text_matches_csi(const char *text)
{
    char normalized[IPCAM_VIDEO_NAME_MAX];
    size_t out = 0;

    if (!text) return 0;
    for (size_t i = 0; text[i] && out + 1 < sizeof(normalized); i++) {
        unsigned char c = (unsigned char)text[i];
        if (isalnum(c)) normalized[out++] = (char)tolower(c);
    }
    normalized[out] = '\0';
    return strstr(normalized, "mx6scsi") != NULL;
}

static int capture_querycap(int fd, struct v4l2_capability *cap,
                            uint32_t *device_caps)
{
    if (!cap || !device_caps) return -1;
    memset(cap, 0, sizeof(*cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, cap) < 0) return -1;
    cap->driver[sizeof(cap->driver) - 1] = '\0';
    cap->card[sizeof(cap->card) - 1] = '\0';
    cap->bus_info[sizeof(cap->bus_info) - 1] = '\0';
    *device_caps = cap->capabilities;
    if (*device_caps & V4L2_CAP_DEVICE_CAPS)
        *device_caps = cap->device_caps;
    return 0;
}

static int capture_has_required_caps(uint32_t device_caps)
{
    return (device_caps & V4L2_CAP_VIDEO_CAPTURE) &&
           (device_caps & V4L2_CAP_STREAMING);
}

static int capture_is_csi_device(const char *sysfs_name,
                                 const struct v4l2_capability *cap)
{
    return capture_text_matches_csi(sysfs_name) ||
           (cap && (capture_text_matches_csi((const char *)cap->driver) ||
                    capture_text_matches_csi((const char *)cap->card)));
}

static void capture_copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t len;
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void capture_log_candidate(const char *path, const char *sysfs_name,
                                  const struct v4l2_capability *cap,
                                  uint32_t device_caps)
{
    MLOGI("video candidate %s: name=%s driver=%s card=%s caps=0x%08x\n",
          path,
          (sysfs_name && sysfs_name[0]) ? sysfs_name : "unknown",
          cap ? (const char *)cap->driver : "unknown",
          cap ? (const char *)cap->card : "unknown",
          device_caps);
}

/*
 * 解析 S_FMT/G_FMT。S_FMT 是驱动接受请求格式的第一确认；出厂 mx6s-csi
 * 的 G_FMT 可能漏回 FourCC、stride 或 sizeimage，因此只对已确认的 CSI
 * 节点使用 S_FMT 回退值，其它设备仍严格拒绝空/错误 FourCC。
 */
static int capture_negotiate_yuyv(ipcam_capture_ctx_t *ctx)
{
    struct v4l2_format fmt;
    uint32_t s_fmt_fourcc;
    uint32_t s_bytes_per_line;
    uint32_t s_size_image;
    uint32_t g_fmt_fourcc;
    uint32_t effective_bytes_per_line;
    uint32_t effective_size_image;
    char s_fourcc_text[5];
    char g_fourcc_text[5];

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = ctx->width;
    fmt.fmt.pix.height = ctx->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        MLOGE("S_FMT YUYV failed: %s\n", strerror(errno));
        return -1;
    }

    s_fmt_fourcc = fmt.fmt.pix.pixelformat;
    s_bytes_per_line = fmt.fmt.pix.bytesperline;
    s_size_image = fmt.fmt.pix.sizeimage;
    capture_fourcc_text(s_fmt_fourcc, s_fourcc_text);
    if (s_fmt_fourcc != V4L2_PIX_FMT_YUYV) {
        MLOGE("S_FMT returned %s(0x%08x), expected YUYV(0x%08x)\n",
              s_fourcc_text, (unsigned int)s_fmt_fourcc,
              (unsigned int)V4L2_PIX_FMT_YUYV);
        return -1;
    }
    if (fmt.fmt.pix.width < 2 || (fmt.fmt.pix.width & 1) ||
        fmt.fmt.pix.height == 0) {
        MLOGE("invalid S_FMT dimensions %ux%u for YUYV 4:2:2\n",
              fmt.fmt.pix.width, fmt.fmt.pix.height);
        return -1;
    }
    if (s_bytes_per_line == 0)
        s_bytes_per_line = fmt.fmt.pix.width * 2U;
    uint64_t s_frame_bytes = (uint64_t)s_bytes_per_line * fmt.fmt.pix.height;
    if (s_bytes_per_line < fmt.fmt.pix.width * 2U ||
        s_frame_bytes > UINT32_MAX) {
        MLOGE("invalid S_FMT stride=%u for %ux%u (frame bytes=%llu)\n",
              s_bytes_per_line, fmt.fmt.pix.width, fmt.fmt.pix.height,
              (unsigned long long)s_frame_bytes);
        return -1;
    }
    if (s_size_image == 0) s_size_image = (uint32_t)s_frame_bytes;
    if (s_size_image < s_frame_bytes) {
        MLOGE("S_FMT sizeimage=%u below stride*height=%llu\n", s_size_image,
              (unsigned long long)s_frame_bytes);
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_G_FMT, &fmt) < 0) {
        MLOGE("G_FMT: %s\n", strerror(errno));
        return -1;
    }

    g_fmt_fourcc = fmt.fmt.pix.pixelformat;
    capture_fourcc_text(g_fmt_fourcc, g_fourcc_text);
    if (g_fmt_fourcc != V4L2_PIX_FMT_YUYV &&
        !(g_fmt_fourcc == 0 && ctx->legacy_gfmt_pixelformat_missing &&
          s_fmt_fourcc == V4L2_PIX_FMT_YUYV)) {
        MLOGE("driver did not honor YUYV(0x%08x) (G_FMT=%s(0x%08x))\n",
              (unsigned int)V4L2_PIX_FMT_YUYV, g_fourcc_text,
              (unsigned int)g_fmt_fourcc);
        return -1;
    }

    if (g_fmt_fourcc == 0) {
        /*
         * 出厂 mx6s-csi 已在 S_FMT 中按 YUYV 配置 CSI，但 G_FMT 直接返回
         * 未填充的 pix 结构。这里仅对已识别的 CSI 放宽，避免掩盖其它设备
         * 真正的像素格式协商失败。
         */
        MLOGW("%s G_FMT returned pixelformat=NONE(0x00000000); "
              "using S_FMT=%s for legacy mx6s-csi\n",
              ctx->device_path, s_fourcc_text);
    }

    ctx->width  = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;
    /* YUYV 4:2:2 按像素对共享色度；奇数宽度会使编码平面丢掉最后一列，
     * 因此在协商阶段直接报告硬件能力不匹配，而不是输出隐性损坏的 JPEG。 */
    if (ctx->width < 2 || (ctx->width & 1) || ctx->height <= 0) {
        MLOGE("unsupported negotiated dimensions %dx%d for YUYV 4:2:2\n",
              ctx->width, ctx->height);
        return -1;
    }
    effective_bytes_per_line = fmt.fmt.pix.bytesperline;
    if (effective_bytes_per_line == 0 && ctx->legacy_gfmt_pixelformat_missing) {
        effective_bytes_per_line = s_bytes_per_line;
        MLOGW("%s G_FMT returned bytesperline=0; using S_FMT bytesperline=%u\n",
              ctx->device_path, effective_bytes_per_line);
    }
    if (effective_bytes_per_line == 0)
        effective_bytes_per_line = (uint32_t)ctx->width * 2U;
    ctx->bytes_per_line = effective_bytes_per_line;
    uint64_t frame_bytes = (uint64_t)ctx->bytes_per_line * (uint64_t)ctx->height;
    if (ctx->bytes_per_line < (uint32_t)ctx->width * 2U ||
        frame_bytes > UINT32_MAX) {
        MLOGE("invalid negotiated stride=%u for %dx%d (frame bytes=%llu)\n",
              ctx->bytes_per_line, ctx->width, ctx->height,
              (unsigned long long)frame_bytes);
        return -1;
    }
    effective_size_image = fmt.fmt.pix.sizeimage;
    if (effective_size_image == 0 && ctx->legacy_gfmt_pixelformat_missing) {
        effective_size_image = s_size_image;
        MLOGW("%s G_FMT returned sizeimage=0; using S_FMT sizeimage=%u\n",
              ctx->device_path, effective_size_image);
    }
    ctx->size_image = effective_size_image;
    if (ctx->size_image == 0)
        ctx->size_image = (uint32_t)frame_bytes;
    if (ctx->size_image < frame_bytes) {
        MLOGE("driver sizeimage=%u below stride*height=%llu\n", ctx->size_image,
              (unsigned long long)frame_bytes);
        return -1;
    }
    ctx->pixel_format = g_fmt_fourcc == 0 ? s_fmt_fourcc : g_fmt_fourcc;
    struct v4l2_streamparm parm;
    uint32_t target_fps = ipcam_param_get_target_fps();
    ctx->target_fps = target_fps;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = target_fps;
    if (xioctl(ctx->fd, VIDIOC_S_PARM, &parm) == 0 &&
        parm.parm.capture.timeperframe.numerator != 0 &&
        parm.parm.capture.timeperframe.denominator != 0) {
        ctx->actual_fps = parm.parm.capture.timeperframe.denominator /
                          parm.parm.capture.timeperframe.numerator;
        /* 驱动返回的时间片可能与目标不同；这时保留真实采集值并由软件选帧，
         * 避免把驱动实际 30 fps 误报为应用输出 15 fps。 */
        ctx->fps_controlled = ctx->actual_fps == target_fps;
    } else {
        /* 驱动拒绝帧率控制时，采集线程会按单调时钟软件选帧。 */
        ctx->fps_controlled = 0;
        ctx->actual_fps = 0;
    }
    MLOGI("camera negotiated: %s %dx%d fmt=%s stride=%u sizeimage=%u\n",
          ctx->device_path, ctx->width, ctx->height,
          g_fmt_fourcc == 0 ? s_fourcc_text : g_fourcc_text,
          ctx->bytes_per_line, ctx->size_image);
    MLOGI("camera frame interval: target=%u actual=%u fps\n",
          target_fps, ctx->actual_fps);
    return 0;
}

/*
 * 按环境/编译配置打开设备并记录身份与格式能力。
 * 自动模式只选择名称/QUERYCAP 能确认是 mx6s-csi 的节点，避免把同时存在
 * 的 PxP 节点当作摄像头；显式覆盖仍保留，方便板级调试其它兼容 V4L2 设备。
 */
static int capture_open_device(ipcam_capture_ctx_t *ctx)
{
    const char *video_dev = getenv("IPCAM_VIDEO_DEV");
    if (!video_dev || !*video_dev) video_dev = IPCAM_VIDEO_DEV;

    ctx->device_path[0] = '\0';
    ctx->legacy_gfmt_pixelformat_missing = 0;
    if (video_dev && *video_dev) {
        struct v4l2_capability cap;
        char sysfs_name[IPCAM_VIDEO_NAME_MAX] = { 0 };
        uint32_t device_caps = 0;
        int fd;

        /* 使用非阻塞 DQBUF，使 capture_stop 可以只改变 service_running；
         * 不必在另一个线程正在 ioctl 时关闭可被系统复用的 fd。 */
        fd = open(video_dev, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            MLOGE("open %s: %s\n", video_dev, strerror(errno));
            return -1;
        }
        if (capture_querycap(fd, &cap, &device_caps) < 0) {
            MLOGE("QUERYCAP %s: %s\n", video_dev, strerror(errno));
            close(fd);
            return -1;
        }
        capture_read_sysfs_name(video_dev, sysfs_name, sizeof(sysfs_name));
        capture_log_candidate(video_dev, sysfs_name, &cap, device_caps);
        if (!capture_has_required_caps(device_caps)) {
            MLOGE("%s lacks video-capture or streaming capability\n", video_dev);
            close(fd);
            return -1;
        }
        if (strlen(video_dev) >= sizeof(ctx->device_path)) {
            MLOGE("configured camera path is too long: %s\n", video_dev);
            close(fd);
            return -1;
        }
        ctx->fd = fd;
        capture_copy_text(ctx->device_path, sizeof(ctx->device_path), video_dev);
        ctx->legacy_gfmt_pixelformat_missing =
            capture_is_csi_device(sysfs_name, &cap);
        MLOGI("camera device selected by override: %s (name=%s driver=%s card=%s)\n",
              ctx->device_path,
              sysfs_name[0] ? sysfs_name : "unknown",
              (const char *)cap.driver, (const char *)cap.card);
    } else {
        capture_video_node_t nodes[IPCAM_MAX_VIDEO_NODES];
        struct v4l2_capability selected_cap;
        char selected_name[IPCAM_VIDEO_NAME_MAX] = { 0 };
        uint32_t selected_caps = 0;
        int node_count;
        int selected_fd = -1;

        node_count = capture_collect_video_nodes(nodes,
                                                 sizeof(nodes) / sizeof(nodes[0]));
        if (node_count < 0) return -1;
        if (node_count == 0) {
            MLOGE("no /dev/videoN nodes found; CSI camera is unavailable\n");
            return -1;
        }

        for (int i = 0; i < node_count; i++) {
            struct v4l2_capability cap;
            char sysfs_name[IPCAM_VIDEO_NAME_MAX] = { 0 };
            uint32_t device_caps = 0;
            int fd;

            fd = open(nodes[i].path, O_RDWR | O_NONBLOCK);
            if (fd < 0) {
                MLOGW("open video candidate %s failed: %s\n",
                      nodes[i].path, strerror(errno));
                continue;
            }
            if (capture_querycap(fd, &cap, &device_caps) < 0) {
                MLOGW("QUERYCAP %s failed: %s\n", nodes[i].path, strerror(errno));
                close(fd);
                continue;
            }
            capture_read_sysfs_name(nodes[i].path, sysfs_name, sizeof(sysfs_name));
            capture_log_candidate(nodes[i].path, sysfs_name, &cap, device_caps);
            if (!capture_has_required_caps(device_caps)) {
                MLOGI("skip %s: not a streaming video-capture node\n", nodes[i].path);
                close(fd);
                continue;
            }
            if (!capture_is_csi_device(sysfs_name, &cap)) {
                MLOGI("skip %s: candidate is not mx6s-csi\n", nodes[i].path);
                close(fd);
                continue;
            }
            if (selected_fd >= 0) {
                /* 节点已按编号排序，保留第一个可用 CSI，避免设备选择不确定。 */
                MLOGW("multiple mx6s-csi nodes found; keeping the lowest-numbered one\n");
                close(fd);
                continue;
            }
            selected_fd = fd;
            selected_cap = cap;
            selected_caps = device_caps;
            capture_copy_text(selected_name, sizeof(selected_name), sysfs_name);
            capture_copy_text(ctx->device_path, sizeof(ctx->device_path), nodes[i].path);
        }

        if (selected_fd < 0) {
            MLOGE("auto discovery found no mx6s-csi capture node; refusing non-CSI video devices\n");
            return -1;
        }
        ctx->fd = selected_fd;
        ctx->legacy_gfmt_pixelformat_missing = 1;
        MLOGI("camera device selected automatically: %s (name=%s driver=%s card=%s caps=0x%08x)\n",
              ctx->device_path,
              selected_name[0] ? selected_name : "unknown",
              (const char *)selected_cap.driver,
              (const char *)selected_cap.card, selected_caps);
    }

    struct v4l2_capability cap;
    uint32_t selected_caps = 0;
    if (capture_querycap(ctx->fd, &cap, &selected_caps) < 0) {
        MLOGE("QUERYCAP selected camera failed: %s\n", strerror(errno));
        capture_teardown(ctx);
        return -1;
    }
    if (!capture_has_required_caps(selected_caps)) {
        MLOGE("selected camera %s lost capture/streaming capability\n",
              ctx->device_path);
        capture_teardown(ctx);
        return -1;
    }
    MLOGI("camera identity: driver=%s card=%s bus=%s\n",
          cap.driver, cap.card, cap.bus_info);
    /* 记录驱动声明的格式清单，避免现场只看到协商失败而不知道设备能力。 */
    for (struct v4l2_fmtdesc desc = {0}; ; desc.index++) {
        char fourcc_text[5];
        desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(ctx->fd, VIDIOC_ENUM_FMT, &desc) < 0) break;
        capture_fourcc_text(desc.pixelformat, fourcc_text);
        MLOGI("camera format[%u]: %s %s\n", desc.index,
              fourcc_text, desc.description);
    }

    if (capture_negotiate_yuyv(ctx) < 0) {
        capture_teardown(ctx);
        return -1;
    }
    return 0;
}

/* 申请并排队 V4L2 mmap 缓冲，同时核验每个 buffer 能容纳协商帧。 */
static int capture_init_mmap(ipcam_capture_ctx_t *ctx)
{
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    memset(&req, 0, sizeof(req));
    req.count = V4L2_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        MLOGE("REQBUFS: %s\n", strerror(errno));
        return -1;
    }
    if (req.count < 2) {
        MLOGE("insufficient buffer memory (got %u)\n", req.count);
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
            MLOGE("QUERYBUF %d: %s\n", i, strerror(errno));
            capture_teardown(ctx);
            return -1;
        }

        ctx->bufs[i].length = buf.length;
        size_t min_frame_bytes = (size_t)ctx->bytes_per_line * (size_t)ctx->height;
        if (ctx->bufs[i].length < min_frame_bytes) {
            MLOGE("V4L2 buffer %d length=%zu below negotiated frame=%zu\n",
                  i, ctx->bufs[i].length, min_frame_bytes);
            capture_teardown(ctx);
            return -1;
        }
        ctx->bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, ctx->fd, buf.m.offset);
        if (ctx->bufs[i].start == MAP_FAILED) {
            MLOGE("mmap buf %d: %s\n", i, strerror(errno));
            ctx->bufs[i].start = NULL;
            capture_teardown(ctx);
            return -1;
        }

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            MLOGE("QBUF %d: %s\n", i, strerror(errno));
            capture_teardown(ctx);
            return -1;
        }
    }
    return 0;
}

/* DQBUF→两路非阻塞广播→QBUF；软件限帧时仍立即归还未选中的 buffer。 */
static void *capture_thread(void *arg)
{
    ipcam_capture_ctx_t *ctx = arg;
    struct v4l2_buffer buf;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned long frames = 0, dropped_disp = 0, dropped_enc = 0;
    struct timeval t0, t1;
    uint64_t next_emit_ns = 0;
    uint64_t emit_interval_ns = ctx->target_fps > 0 ?
        1000000000ULL / ctx->target_fps : 66666666ULL;

    MLOGI("capture thread start\n");
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        MLOGE("STREAMON: %s\n", strerror(errno));
        return NULL;
    }
    /* BCF2 会在媒体线程真正进入工作态时再打一条日志，便于区分“线程创建成功”和“首帧可用”。 */
    MLOGI("capture stream on: device=%s buffers=%d target_fps=%u actual_fps=%u controlled=%d\n",
          ctx->device_path, ctx->n_bufs, ctx->target_fps, ctx->actual_fps,
          ctx->fps_controlled);

    gettimeofday(&t0, NULL);

    while (*ctx->running && ctx->service_running) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                /* 非阻塞设备没有新帧时让出 CPU；停止标志会在下一轮被观察。 */
                usleep(1000);
                continue;
            }
            /* EIO = 硬件错误（CSI 接触不良 / 传感器故障），不是 EAGAIN 的可重试变体 */
            MLOGE("DQBUF fatal: %s\n", strerror(errno));
            break;
        }

        /* V4L2_BUF_FLAG_ERROR：驱动说这一帧坏了，丢弃但仍 QBUF */
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            MLOGW("frame %u marked BUF_FLAG_ERROR, dropping\n", buf.sequence);
            if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
                MLOGE("QBUF after error: %s\n", strerror(errno));
                break;
            }
            continue;
        }

        if (buf.index >= (unsigned int)ctx->n_bufs) {
            MLOGE("DQBUF returned invalid index %u (n=%d)\n", buf.index, ctx->n_bufs);
            break;
        }

        if (!ctx->fps_controlled) {
            struct timespec now;
            uint64_t now_ns = 0;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
                now_ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
            if (next_emit_ns && now_ns < next_emit_ns) {
                /* 仍需 QBUF，及时把未选中的驱动 buffer 归还。 */
                if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
                    MLOGE("QBUF software-rate drop: %s\n", strerror(errno));
                    break;
                }
                continue;
            }
            next_emit_ns = now_ns ? now_ns + emit_interval_ns : 0;
        }

        size_t min_frame_bytes = (size_t)ctx->bytes_per_line * (size_t)ctx->height;
        if (buf.bytesused >= min_frame_bytes &&
            buf.bytesused <= ctx->bufs[buf.index].length) {
            const void *src = ctx->bufs[buf.index].start;
            /* 这里只记录当前帧是否被某路 ring 丢弃，计数本身由 uint64_t
             * 累加器保存；用 int 表示布尔事件，避免日志格式把小整型误当成
             * 64 位计数，且让 -Wformat=2 能在交叉编译时直接拦住问题。 */
            int drop_disp_now = 0, drop_enc_now = 0;

            /* 双路非阻塞写：任一满则丢该路（不阻塞生产者、不等消费者） */
            ipcam_frame_meta_t meta;
            memset(&meta, 0, sizeof(meta));
            /* 两路广播必须共享同一个采集时间戳，否则录像时间轴与预览/直播
             * 会因分别调用 clock_gettime 而产生不可解释的微小偏差。 */
            struct timespec captured_at;
            if (clock_gettime(CLOCK_MONOTONIC, &captured_at) == 0) {
                meta.monotonic_ns = (uint64_t)captured_at.tv_sec * 1000000000ULL +
                                    (uint64_t)captured_at.tv_nsec;
            }
            meta.width = (uint16_t)ctx->width;
            meta.height = (uint16_t)ctx->height;
            meta.stride = ctx->bytes_per_line;
            meta.pixel_format = ctx->pixel_format;
            meta.config_generation = ipcam_param_get_generation();
            if (ctx->rb_disp) {
                if (ipcam_ring_try_append_latest_meta(ctx->rb_disp, src, buf.bytesused, &meta) != 0) {
                    dropped_disp++;
                    drop_disp_now = 1;
                }
            }
            if (ctx->rb_enc) {
                if (ipcam_ring_try_append_meta(ctx->rb_enc, src, buf.bytesused, &meta) != 0) {
                    dropped_enc++;
                    drop_enc_now = 1;
                }
            }
            capture_add_stats(ctx, 1, drop_disp_now, drop_enc_now);
            frames++;
            /*
             * 采集链路不能只靠主循环的 5 秒汇总诊断。沿用 BCF2“首批帧 + 周期帧”
             * 策略：首 30 帧帮助定位首帧时序，之后每 30 帧报告一次序号、时间戳、
             * 有效长度和两路广播是否丢帧；不按每帧刷屏，避免日志反过来拖慢 CSI。
             */
            if (frames <= 30 || (frames % 30) == 0) {
                MLOGI("frame no=%lu v4l2_seq=%u ts=%llu bytes=%u drop_disp=%d drop_enc=%d\n",
                      frames, buf.sequence, (unsigned long long)meta.monotonic_ns,
                      buf.bytesused, drop_disp_now, drop_enc_now);
            }
        } else if (buf.bytesused < min_frame_bytes) {
            MLOGW("bytesused %u below stride*height %zu, dropping\n",
                  buf.bytesused, min_frame_bytes);
        } else if (buf.bytesused > ctx->bufs[buf.index].length) {
            MLOGW("bytesused %u > buffer length %zu, dropping\n",
                  buf.bytesused, ctx->bufs[buf.index].length);
        }

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            MLOGE("QBUF: %s\n", strerror(errno));
            break;
        }
    }

    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

    gettimeofday(&t1, NULL);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    MLOGI("capture thread exit, frames=%lu drop_disp=%lu drop_enc=%lu avg_fps=%.1f\n",
          frames, dropped_disp, dropped_enc, sec > 0 ? frames / sec : 0);
    return NULL;
}

/* 完成设备协商、容量校验并启动采集线程。 */
int ipcam_capture_start(ipcam_capture_ctx_t *ctx,
                        ipcam_ring_buffer_t *rb_disp,
                        ipcam_ring_buffer_t *rb_enc,
                        volatile sig_atomic_t *running)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->width  = IPCAM_CAPTURE_WIDTH;
    ctx->height = IPCAM_CAPTURE_HEIGHT;
    ctx->rb_disp = rb_disp;
    ctx->rb_enc  = rb_enc;
    ctx->running = running;
    ctx->service_running = 1;
    pthread_mutex_init(&ctx->stats_mtx, NULL);

    if (capture_open_device(ctx) < 0) {
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    /*
     * 先用协商后的 stride/height 校验环槽容量。旧实现按配置分辨率分配
     * 环槽，驱动若返回带 padding 或更高尺寸时会把帧静默丢掉；现在在
     * 启动阶段明确失败，让上层报告“硬件能力不匹配”，避免伪装成直播正常。
     */
    /* 两个分支先统一为 size_t，避免 32 位 ARM 上有符号宽度参与条件表达式。 */
    size_t bytes_per_line = ctx->bytes_per_line ? (size_t)ctx->bytes_per_line :
                              (size_t)ctx->width * 2U;
    size_t min_frame_bytes = bytes_per_line * (size_t)ctx->height;
    /* ring 槽还必须能容纳驱动声明的 sizeimage；只按 stride*height
     * 校验会在 bytesused 带额外尾部时静默丢帧。 */
    size_t required_capacity = min_frame_bytes;
    if ((size_t)ctx->size_image > required_capacity)
        required_capacity = (size_t)ctx->size_image;
    if (!ctx->rb_disp || !ctx->rb_enc ||
        ipcam_ring_capacity(ctx->rb_disp) < required_capacity ||
        ipcam_ring_capacity(ctx->rb_enc) < required_capacity) {
        MLOGE("negotiated frame needs %zu bytes, ring capacity is %zu/%zu\n",
              required_capacity,
              ipcam_ring_capacity(ctx->rb_disp), ipcam_ring_capacity(ctx->rb_enc));
        capture_teardown(ctx);
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    if (capture_init_mmap(ctx) < 0) {
        capture_teardown(ctx);
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }

    if (pthread_create(&ctx->thread, NULL, capture_thread, ctx) != 0) {
        MLOGE("pthread_create capture failed\n");
        capture_teardown(ctx);
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    MLOGI("capture service ready: device=%s format=0x%08x %dx%d stride=%u sizeimage=%u\n",
          ctx->device_path, ctx->pixel_format, ctx->width, ctx->height,
          ctx->bytes_per_line, ctx->size_image);
    return 0;
}

/* 独立停止采集服务，不修改共享全局 running 标志。 */
void ipcam_capture_stop(ipcam_capture_ctx_t *ctx)
{
    if (!ctx) return;

    MLOGI("capture stop requested: device=%s\n", ctx->device_path);
    /* 非阻塞 DQBUF 让线程自行观察 service_running；不从外部关闭 fd，
     * 避免另一个线程正处于 ioctl 时 fd 号被复用造成误操作。 */
    ctx->service_running = 0;
    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }

    /* 线程已退出后再释放 mmap 和设备 fd。 */
    capture_teardown(ctx);
    pthread_mutex_destroy(&ctx->stats_mtx);
    MLOGI("capture stopped: device=%s\n", ctx->device_path);
}

void ipcam_capture_get_dimensions(const ipcam_capture_ctx_t *ctx, int *w, int *h)
{
    if (!ctx) return;
    if (w) *w = ctx->width;
    if (h) *h = ctx->height;
}

/* 复制采集统计快照；统计锁只覆盖计数，不阻塞 V4L2 DQBUF。 */
void ipcam_capture_get_stats(ipcam_capture_ctx_t *ctx, uint64_t *emitted,
                             uint64_t *dropped_disp, uint64_t *dropped_enc)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    if (emitted) *emitted = ctx->frames_emitted;
    if (dropped_disp) *dropped_disp = ctx->frames_dropped_disp;
    if (dropped_enc) *dropped_enc = ctx->frames_dropped_enc;
    pthread_mutex_unlock(&ctx->stats_mtx);
}
