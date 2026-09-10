#ifndef IPCAM_RECORD_H
#define IPCAM_RECORD_H

/*
 * 板端存储服务：从编码器专用 JPEG 队列消费帧，维护录像状态机，
 * 将完整 MJPEG AVI 段和独立照片安全写入经过挂载校验的 SD 卡目录。
 * 录像线程与控制/HTTP 调用方通过互斥锁、条件变量和复制快照协作，
 * 不把 ring 槽内存交给异步文件写入。
 */

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>

#include "ipcam_ringbuffer.h"

/* 录像状态是 GUI、HTTP 和 camctl 共用的稳定契约。 */
typedef enum {
    IPCAM_RECORD_IDLE = 0,
    IPCAM_RECORD_STARTING,
    IPCAM_RECORD_RECORDING,
    IPCAM_RECORD_STOPPING,
    IPCAM_RECORD_ERROR
} ipcam_record_state_t;

typedef struct ipcam_record_status_s {
    ipcam_record_state_t state;
    uint32_t segment_no;
    uint64_t frame_count;
    uint64_t real_frames;
    uint64_t repeated_frames;
    uint64_t bytes_written;
    uint64_t elapsed_ms;
    char current_file[256];
    char last_error[128];
} ipcam_record_status_t;

typedef struct ipcam_record_perf_s {
    uint64_t frame_count;
    uint64_t real_frames;
    uint64_t repeated_frames;
    uint64_t bytes_written;
    int storage_mounted;
    uint64_t storage_available_bytes;
    uint64_t storage_last_probe_ns;
} ipcam_record_perf_t;

typedef struct ipcam_record_ctx_s {
    ipcam_ring_buffer_t *jpeg_rb;
    volatile sig_atomic_t *running;
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_mutex_t latest_mtx;
    pthread_cond_t latest_cond;
    int stop_requested;
    int shutdown_requested;
    ipcam_record_status_t status;
    char storage_root[256];
    int width;
    int height;
    int fps;
    uint64_t recording_start_ns; /* 当前一次手动录像的总起点，跨分段不清零 */
    uint64_t frames_written_total; /* 含当前活动段，供性能统计使用 */
    uint64_t real_frames_written_total;
    uint64_t repeated_frames_total;
    uint64_t bytes_written_total;
    int storage_mounted_cached;
    uint64_t storage_available_cached;
    uint64_t storage_last_probe_ns;
    uint64_t storage_bytes_since_probe;
    int storage_probe_failed;
    char storage_last_error[128];
    unsigned long session_id;
    void *segment; /* 私有 AVI 段状态，避免把格式细节暴露给调用方。 */
    unsigned char *latest_jpeg; /* 独立快照缓存，避免录像消费者取走后拍照无帧 */
    size_t latest_size;
    size_t latest_cap;
    unsigned long latest_seq;
} ipcam_record_ctx_t;

/* 启动录像消费者线程；不会自动开始写文件，初始状态保持 IDLE。 */
int ipcam_record_start(ipcam_record_ctx_t *ctx, ipcam_ring_buffer_t *jpeg_rb,
                       volatile sig_atomic_t *running, const char *storage_root,
                       int width, int height, int fps);

/* 异步请求开始/停止；实际状态以 ipcam_record_get_status 为准。 */
int ipcam_record_request_start(ipcam_record_ctx_t *ctx);
int ipcam_record_request_stop(ipcam_record_ctx_t *ctx);
/* 保存录像队列中的下一张最新 JPEG；与录像并行，不切换摄像头规格。 */
int ipcam_record_save_photo(ipcam_record_ctx_t *ctx, char *path_out, size_t path_sz);
void ipcam_record_get_status(ipcam_record_ctx_t *ctx, ipcam_record_status_t *out);
/* 查询与录像/拍照相同的挂载身份和可用空间；未挂载时空间返回 0。 */
int ipcam_record_get_storage_status(ipcam_record_ctx_t *ctx,
                                    int *mounted, uint64_t *available_bytes);
/* 读取当前录像线程已写入的帧/字节累计值，包含尚未完成收尾的活动段。 */
void ipcam_record_get_metrics(ipcam_record_ctx_t *ctx, uint64_t *frames,
                              uint64_t *bytes);
/* 复制录像真实/重复帧和存储缓存；查询不触发 statvfs。 */
void ipcam_record_get_perf(ipcam_record_ctx_t *ctx, ipcam_record_perf_t *out);

/* 关闭线程并完成正在写入的 AVI 段；调用后不得再访问 ctx。 */
void ipcam_record_stop(ipcam_record_ctx_t *ctx);

#endif /* IPCAM_RECORD_H */
