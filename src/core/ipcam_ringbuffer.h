#ifndef IPCAM_RING_BUFFER_H
#define IPCAM_RING_BUFFER_H

/*
 * 多生产者单消费者环形缓冲（基于 BCF2 libmoringbuffer 模式，简化）。
 *
 * 本期典型用法（1 个 writer + 1 个 reader）：
 *   - capture_thread (writer) -> rb_yuyv -> display_thread (reader)
 *   - capture_thread (writer) -> rb_yuyv -> encode_thread  (reader)
 *
 * **不允许多个 reader 同时挂同一个 rb**——这是调用方约定。
 * 多消费者场景请开多个 ring（每个 ring 单 reader），例如 rb_yuyv_display / rb_yuyv_encode。
 *
 * 帧布局：
 *   每个 slot = [ipcam_frame_t header (24B)] [payload bytes]
 *   header.rawData 指向 slot 的 payload 起点
 *   header.size    是 payload 的字节数
 *
 * 同步：
 *   pthread_mutex + pthread_cond(CLOCK_MONOTONIC)
 */

#include <pthread.h>
#include <stddef.h>

#define IPCAM_FRAME_TYPE_I   1   /* MJPEG: 每帧都是 I */
#define IPCAM_FRAME_TYPE_P   2   /* 预留（H.264 用） */

typedef struct ipcam_frame_s {
    void    *rawData;      /* 帧数据指针（指向 slot 内的 payload） */
    size_t   size;         /* 帧字节数 */
    unsigned long seqNo;   /* 单调递增序列号 */
    int      type;         /* IPCAM_FRAME_TYPE_* */
} ipcam_frame_t;

/*
 * 内部 slot 头。每个 slot 在 alloc 时按以下布局连续分配：
 *   [ipcam_slot_t hdr] [payload 字节...]
 * 这样 rawData 指向 hdr 之后的 payload；payload 不会被帧头覆盖。
 */
typedef struct ipcam_slot_s {
    ipcam_frame_t header;  /* 帧头：rawData 指向 slot 内的 payload */
} ipcam_slot_t;

#define IPCAM_SLOT_PAYLOAD_OFF  sizeof(ipcam_slot_t)

typedef struct ipcam_ring_buffer_s {
    /* slot 数组：每槽 = [ipcam_slot_t][payload] */
    char    **slot_mem;    /* slot_mem[i] 指向第 i 个 slot 的起始（含 hdr） */
    size_t    slot_stride; /* sizeof(ipcam_slot_t) + slot_bytes */
    size_t    slot_bytes;  /* payload 字节数 */
    int       depth;       /* 槽数 */

    /* 共享状态 */
    pthread_mutex_t mtx;
    pthread_cond_t  cond_not_empty;
    pthread_cond_t  cond_not_full;

    int       write_idx;       /* 下一个写入 slot */
    int       read_idx;        /* 下一个读取 slot */
    int       count;           /* 当前已占用槽数 */
    int       closed;          /* 1 = 关闭（生产者退出） */

    unsigned long seq_counter;
} ipcam_ring_buffer_t;

/*
 * 创建环形缓冲。
 *   depth      = 槽数
 *   slot_bytes = 单帧 payload 最大字节数（YUYV 640x480 = 614400）
 *
 * 返回非 NULL 成功，NULL 失败。
 */
ipcam_ring_buffer_t *ipcam_ring_create(int depth, size_t slot_bytes);

/* 销毁环形缓冲 */
void ipcam_ring_destroy(ipcam_ring_buffer_t *rb);

/*
 * 生产者写入一帧：阻塞直到有空闲槽或缓冲被关闭。
 * in_data 指向外部 buffer（in_bytes 字节），函数 memcpy 到 slot payload。
 * 返回 0=成功，-1=已关闭。
 */
int ipcam_ring_append(ipcam_ring_buffer_t *rb, const void *in_data, size_t in_bytes);

/*
 * 生产者非阻塞写入：满则立即返回 -1，不阻塞生产者。
 * 用于实时数据流（如 V4L2 采集线程）——满则丢当前帧，不阻塞采集。
 * 返回 0=成功，-1=已关闭或满。
 */
int ipcam_ring_try_append(ipcam_ring_buffer_t *rb, const void *in_data, size_t in_bytes);

/*
 * 消费者取一帧：阻塞直到有可用帧或缓冲被关闭且清空。
 * out_frame 由函数填充；rawData/size/seqNo/type 指向 slot 内的内容。
 * 调用者必须在调用 ipcam_ring_release 之前不修改 rawData 所指的 payload。
 * 返回 0=成功，-1=已关闭且无帧。
 */
int ipcam_ring_get(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame);

/* 消费者消费完一帧，slot 进入空闲 */
void ipcam_ring_release(ipcam_ring_buffer_t *rb);

/* 标记缓冲关闭；唤醒所有阻塞线程 */
void ipcam_ring_close(ipcam_ring_buffer_t *rb);

/* 当前帧数（仅供统计） */
int ipcam_ring_count(ipcam_ring_buffer_t *rb);

/*
 * 工具：给定 frame.rawData，反查它属于哪个 rb 的第几个 slot。
 * 仅在同一进程内有效（指针比较）。返回 -1 表示不在本 rb 内。
 */
int ipcam_ring_slot_index(const ipcam_ring_buffer_t *rb, const void *raw_data);

#endif /* IPCAM_RING_BUFFER_H */