#ifndef IPCAM_CAPTURE_H
#define IPCAM_CAPTURE_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include <stddef.h>     /* size_t */
#include <stdint.h>
#include "ipcam_ringbuffer.h"

#define IPCAM_CAPTURE_DEVICE_PATH_MAX 128

/*
 * V4L2 capture thread.
 * - 未指定 IPCAM_VIDEO_DEV 时扫描 /dev/videoN，并选择已识别的 mx6s-csi
 * - 显式指定 IPCAM_VIDEO_DEV 时仍执行 capability、格式和帧容量校验
 * - 协商 YUYV 4:2:2 @ IPCAM_CAPTURE_WIDTH x IPCAM_CAPTURE_HEIGHT
 * - 用 mmap 申请 N 个 video buffer，循环 DQBUF -> 拷贝到两条环形缓冲 -> QBUF
 *
 * 写两条 rb_yuyv_disp / rb_yuyv_enc 给 display 与 encode 各自独立消费；
 * 任意一条满则丢该路（消费者堵住了，我们不等）。
 */
#include "ipcam_ringbuffer.h"

typedef struct ipcam_capture_ctx_s {
    int              fd;             /* 已选择的 V4L2 设备 fd */
    char             device_path[IPCAM_CAPTURE_DEVICE_PATH_MAX];
    /*
     * 正点原子 4.1.15 的 mx6s-csi 驱动在 S_FMT 中接受了 YUYV，却可能在
     * G_FMT 中遗漏 pixelformat/布局字段。只有确认设备身份属于 mx6s-csi
     * 时才启用兼容回退，避免把其它 V4L2 设备的错误格式当成 YUYV。
     */
    int              legacy_gfmt_pixelformat_missing;
    int              width;
    int              height;
    uint32_t         bytes_per_line; /* V4L2 实际行跨度 */
    uint32_t         size_image;     /* V4L2 报告的单帧有效上限 */
    uint32_t         pixel_format;   /* 协商后的 V4L2 fourcc */
    uint32_t         target_fps;     /* 请求的输出目标帧率 */
    uint32_t         actual_fps;
    int              fps_controlled; /* 1=驱动精确接受目标，0=需软件选帧或驱动未返回精确值 */
    int              n_bufs;         /* V4L2 缓冲数量（建议 >= 3） */
    struct v4l2_buf_info {
        void         *start;
        size_t        length;
    } *bufs;

    ipcam_ring_buffer_t *rb_disp;     /* 写入端 #1（display 消费） */
    ipcam_ring_buffer_t *rb_enc;      /* 写入端 #2（encode 消费） */
    volatile sig_atomic_t *running;
    volatile sig_atomic_t service_running; /* 仅采集服务自身的生命周期 */
    pthread_t        thread;          /* 非 detached，可 join */
    pthread_mutex_t  stats_mtx;
    uint64_t         frames_emitted;
    uint64_t         frames_dropped_disp;
    uint64_t         frames_dropped_enc;
} ipcam_capture_ctx_t;

/* 初始化并启动采集线程（pthread_create 后立即返回） */
int  ipcam_capture_start(ipcam_capture_ctx_t *ctx,
                         ipcam_ring_buffer_t *rb_disp,
                         ipcam_ring_buffer_t *rb_enc,
                         volatile sig_atomic_t *running);

/* 通知线程退出 + pthread_join + 释放 V4L2 资源 */
void ipcam_capture_stop(ipcam_capture_ctx_t *ctx);

/* 查询 V4L2 协商后的实际分辨率（在 capture_start 成功后调用） */
void ipcam_capture_get_dimensions(const ipcam_capture_ctx_t *ctx, int *w, int *h);
/* 读取采集输出与两路环槽丢帧累计值；返回前会复制快照，调用方无需持锁。 */
void ipcam_capture_get_stats(ipcam_capture_ctx_t *ctx, uint64_t *emitted,
                             uint64_t *dropped_disp, uint64_t *dropped_enc);

#endif /* IPCAM_CAPTURE_H */
