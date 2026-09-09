#define _GNU_SOURCE
#include "ipcam_capture.h"
#include "ipcam_log.h"

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
#include <unistd.h>

#include "ipcam_config.h"

#define V4L2_BUFS  4

static int xioctl(int fd, int req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

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
    MLOGI("camera negotiated: %dx%d fmt=YUYV\n", ctx->width, ctx->height);
    return 0;
}

static int capture_open_device(ipcam_capture_ctx_t *ctx)
{
    struct v4l2_capability cap;
    ctx->fd = open(IPCAM_VIDEO_DEV, O_RDWR);
    if (ctx->fd < 0) {
        MLOGE("open %s: %s\n", IPCAM_VIDEO_DEV, strerror(errno));
        return -1;
    }

    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        MLOGE("QUERYCAP: %s\n", strerror(errno));
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        MLOGE("%s is not a capture device\n", IPCAM_VIDEO_DEV);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    if (capture_negotiate_yuyv(ctx) < 0) {
        close(ctx->fd);
        ctx->fd = -1;
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

static void *capture_thread(void *arg)
{
    ipcam_capture_ctx_t *ctx = arg;
    struct v4l2_buffer buf;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned long frames = 0, dropped_disp = 0, dropped_enc = 0;
    struct timeval t0, t1;

    MLOGI("capture thread start\n");
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        MLOGE("STREAMON: %s\n", strerror(errno));
        return NULL;
    }

    gettimeofday(&t0, NULL);

    while (*ctx->running) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
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

        if (buf.bytesused > 0 && buf.bytesused <= ctx->bufs[buf.index].length) {
            const void *src = ctx->bufs[buf.index].start;

            /* 双路非阻塞写：任一满则丢该路（不阻塞生产者、不等消费者） */
            if (ctx->rb_disp) {
                if (ipcam_ring_try_append(ctx->rb_disp, src, buf.bytesused) != 0)
                    dropped_disp++;
            }
            if (ctx->rb_enc) {
                if (ipcam_ring_try_append(ctx->rb_enc, src, buf.bytesused) != 0)
                    dropped_enc++;
            }
            frames++;
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

    if (capture_open_device(ctx) < 0) return -1;
    if (capture_init_mmap(ctx) < 0) {
        capture_teardown(ctx);
        return -1;
    }

    if (pthread_create(&ctx->thread, NULL, capture_thread, ctx) != 0) {
        MLOGE("pthread_create capture failed\n");
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

    /* 2) 关 fd（让 DQBUF 立刻返回 EBADF，避免依赖 STREAMOFF 的不可靠唤醒） */
    if (ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        close(ctx->fd);   /* EBADF 让 DQBUF 立即返回 */
        ctx->fd = -1;
    }

    /* 3) join（线程不会再卡在 DQBUF，因为 fd 已关） */
    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }

    /* 4) 释放 mmap（capture_teardown 看到 fd==-1 会跳过 close） */
    capture_teardown(ctx);
}

void ipcam_capture_get_dimensions(const ipcam_capture_ctx_t *ctx, int *w, int *h)
{
    if (!ctx) return;
    if (w) *w = ctx->width;
    if (h) *h = ctx->height;
}