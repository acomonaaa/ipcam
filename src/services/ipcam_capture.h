#ifndef IPCAM_CAPTURE_H
#define IPCAM_CAPTURE_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include <stddef.h>     /* size_t */
#include "ipcam_ringbuffer.h"
#include "ipcam_light.h"

#define IPCAM_CAPTURE_DEVICE_PATH_MAX 128

/*
 * V4L2 capture thread.
 * - 默认扫描 /dev/videoN，按 sysfs/V4L2 名称选择 mx6s-csi；也支持配置固定节点
 * - 协商 packed 4:2:2 @ 调用方传入的采集尺寸
 *   （当前出厂 OV5640 默认使用 YUYV；驱动不认则启动失败）
 * - 用 mmap 申请 N 个 video buffer，循环 DQBUF -> 拷贝到两条环形缓冲 -> QBUF
 *
 * 写两条 rb_yuyv_disp / rb_yuyv_enc 给 display 与 encode 各自独立消费；
 * 任意一条满则丢该路（消费者堵住了，我们不等）。
 */
typedef struct ipcam_capture_ctx_s {
    int              fd;             /* 已选择的 V4L2 设备 fd */
    char             device_path[IPCAM_CAPTURE_DEVICE_PATH_MAX];
    /*
     * 正点原子 4.1.15 的 mx6s-csi 驱动在 S_FMT 中保存了尺寸，却遗漏了
     * csi_dev->pix.pixelformat；因此 G_FMT 可能返回 0。只对已确认的
     * mx6s-csi 节点启用这个兼容标记，避免放宽其它设备的格式校验。
     */
    int              legacy_gfmt_pixelformat_missing;
    int              width;
    int              height;
    size_t           bytesperline;  /* 协商后的 packed 4:2:2 每行字节数 */
    size_t           frame_bytes;   /* 协商后的完整帧字节数（sizeimage） */
    int              n_bufs;         /* V4L2 缓冲数量（建议 >= 3） */
    struct v4l2_buf_info {
        void         *start;
        size_t        length;
    } *bufs;

    ipcam_ring_buffer_t *rb_disp;     /* 写入端 #1（display 消费） */
    ipcam_ring_buffer_t *rb_enc;      /* 写入端 #2（encode 消费） */
    ipcam_light_ctx_t light;           /* 借用 video fd 的安全补光控制 */
    volatile sig_atomic_t *running;
    pthread_t        thread;          /* 非 detached，可 join */
} ipcam_capture_ctx_t;

/* 初始化并启动采集线程（pthread_create 后立即返回） */
int  ipcam_capture_start(ipcam_capture_ctx_t *ctx,
                         ipcam_ring_buffer_t *rb_disp,
                         ipcam_ring_buffer_t *rb_enc,
                         int width,
                         int height,
                         volatile sig_atomic_t *running);

/* 通知线程退出 + pthread_join + 释放 V4L2 资源 */
void ipcam_capture_stop(ipcam_capture_ctx_t *ctx);

/* 查询 V4L2 协商后的实际分辨率（在 capture_start 成功后调用） */
void ipcam_capture_get_dimensions(const ipcam_capture_ctx_t *ctx, int *w, int *h);

#endif /* IPCAM_CAPTURE_H */
