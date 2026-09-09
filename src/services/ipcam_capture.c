#define _GNU_SOURCE
#include "ipcam_capture.h"
#include "ipcam_log.h"
#include "ipcam_param.h"

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

/* 解析 S_FMT/G_FMT；要求 YUYV，否则失败 */
/* 协商 YUYV、stride 和帧率；驱动不接受 S_PARM 时标记为软件选帧。 */
static int capture_negotiate_yuyv(ipcam_capture_ctx_t *ctx)
{
    struct v4l2_format fmt;
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

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_G_FMT, &fmt) < 0) {
        MLOGE("G_FMT: %s\n", strerror(errno));
        return -1;
    }

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        MLOGE("driver did not honor YUYV (negotiated %.4s)\n",
              (char *)&fmt.fmt.pix.pixelformat);
        return -1;
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
    ctx->bytes_per_line = fmt.fmt.pix.bytesperline;
    if (ctx->bytes_per_line == 0) ctx->bytes_per_line = (uint32_t)ctx->width * 2U;
    uint64_t frame_bytes = (uint64_t)ctx->bytes_per_line * (uint64_t)ctx->height;
    if (ctx->bytes_per_line < (uint32_t)ctx->width * 2U ||
        frame_bytes > UINT32_MAX) {
        MLOGE("invalid negotiated stride=%u for %dx%d (frame bytes=%llu)\n",
              ctx->bytes_per_line, ctx->width, ctx->height,
              (unsigned long long)frame_bytes);
        return -1;
    }
    ctx->size_image = fmt.fmt.pix.sizeimage;
    if (ctx->size_image == 0)
        ctx->size_image = (uint32_t)frame_bytes;
    if (ctx->size_image < frame_bytes) {
        MLOGE("driver sizeimage=%u below stride*height=%llu\n", ctx->size_image,
              (unsigned long long)frame_bytes);
        return -1;
    }
    ctx->pixel_format = fmt.fmt.pix.pixelformat;
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
    MLOGI("camera negotiated: %dx%d fmt=YUYV stride=%u sizeimage=%u\n",
          ctx->width, ctx->height, ctx->bytes_per_line, ctx->size_image);
    MLOGI("camera frame interval: target=%u actual=%u fps\n",
          target_fps, ctx->actual_fps);
    return 0;
}

/* 按环境/编译配置打开设备并记录身份与格式能力，拒绝非 streaming 节点。 */
static int capture_open_device(ipcam_capture_ctx_t *ctx)
{
    struct v4l2_capability cap;
    const char *video_dev = getenv("IPCAM_VIDEO_DEV");
    if (!video_dev || !*video_dev) video_dev = IPCAM_VIDEO_DEV;
    /* 使用非阻塞 DQBUF，使 capture_stop 可以只改变本服务标志并 join，
     * 不必在另一个线程正在 ioctl 时关闭可被系统复用的 fd。 */
    ctx->fd = open(video_dev, O_RDWR | O_NONBLOCK);
    if (ctx->fd < 0) {
        MLOGE("open %s: %s\n", video_dev, strerror(errno));
        return -1;
    }
    memset(&cap, 0, sizeof(cap));

    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        MLOGE("QUERYCAP: %s\n", strerror(errno));
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        MLOGE("%s is not a capture device\n", video_dev);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        MLOGE("%s does not support streaming capture\n", video_dev);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    MLOGI("camera identity: driver=%s card=%s bus=%s\n",
          cap.driver, cap.card, cap.bus_info);
    /* 记录驱动声明的格式清单，第一阶段以实测 YUYV 档位为准而非猜测。 */
    for (struct v4l2_fmtdesc desc = {0}; ; desc.index++) {
        desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(ctx->fd, VIDIOC_ENUM_FMT, &desc) < 0) break;
        MLOGI("camera format[%u]: %.4s %s\n", desc.index,
              (char *)&desc.pixelformat, desc.description);
    }

    if (capture_negotiate_yuyv(ctx) < 0) {
        close(ctx->fd);
        ctx->fd = -1;
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
            uint64_t drop_disp_now = 0, drop_enc_now = 0;

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
    return 0;
}

/* 独立停止采集服务，不修改共享全局 running 标志。 */
void ipcam_capture_stop(ipcam_capture_ctx_t *ctx)
{
    if (!ctx) return;

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
