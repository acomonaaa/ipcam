#ifndef IPCAM_RING_BUFFER_H
#define IPCAM_RING_BUFFER_H

/*
 * 多生产者单消费者环形缓冲（基于 BCF2 libmoringbuffer 模式，简化）。
 *
 * 本期典型用法（1 个 writer + 1 个 reader）：
 *   - capture_thread (writer) -> rb_yuyv -> display_thread (reader)
 *   - capture_thread (writer) -> rb_yuyv -> encode_thread  (reader)
 *
 * 消费队列仍是单 reader；需要多个网络观察者时使用 copy_latest() 的只读
 * 快照接口，不移动 read_idx。多消费者写入场景请开多个 ring，例如
 * rb_yuyv_display / rb_yuyv_encode / rb_jpeg_record。
 *
 * 帧布局：
 *   每个 slot = [ipcam_frame_t header] [payload bytes]
 *   header.rawData 指向 slot 的 payload 起点
 *   header.size    是 payload 的字节数
 *
 * 同步：
 *   pthread_mutex + pthread_cond(CLOCK_MONOTONIC)
 */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define IPCAM_FRAME_TYPE_I   1   /* MJPEG: 每帧都是 I */
#define IPCAM_FRAME_TYPE_P   2   /* 预留（H.264 用） */
/* 显示预览帧的 fourcc（对应 V4L2 RGBP，避免服务层散落魔数）。 */
#define IPCAM_PIXEL_FORMAT_RGB565  ((uint32_t)('R') | ((uint32_t)('G') << 8) | \
                                    ((uint32_t)('B') << 16) | ((uint32_t)('P') << 24))

typedef struct ipcam_frame_s {
    void    *rawData;      /* 帧数据指针（指向 slot 内的 payload） */
    size_t   size;         /* 帧字节数 */
    unsigned long seqNo;   /* 单调递增序列号 */
    int      type;         /* IPCAM_FRAME_TYPE_* */
    uint64_t monotonic_ns; /* 采集/编码完成时的 CLOCK_MONOTONIC 时间戳 */
    uint16_t width;        /* 有效图像宽度；编码后仍保留源尺寸 */
    uint16_t height;       /* 有效图像高度 */
    uint32_t stride;       /* 原始帧行跨度，压缩帧为 0 */
    uint32_t pixel_format; /* V4L2 fourcc；压缩帧为 0 */
    uint32_t config_generation; /* 参数切换代次，便于消费者丢弃旧帧 */
} ipcam_frame_t;

typedef struct ipcam_frame_meta_s {
    uint64_t monotonic_ns;
    uint16_t width;
    uint16_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t config_generation;
} ipcam_frame_meta_t;

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
    uint64_t dropped_count;      /* 覆盖或主动跳过的过期帧数，供媒体链路监控 */
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
int ipcam_ring_append_meta(ipcam_ring_buffer_t *rb, const void *in_data,
                           size_t in_bytes, const ipcam_frame_meta_t *meta);

/*
 * 生产者非阻塞写入：满则立即返回 -1，不阻塞生产者。
 * 用于实时数据流（如 V4L2 采集线程）——满则丢当前帧，不阻塞采集。
 * 返回 0=成功，-1=已关闭或满。
 */
int ipcam_ring_try_append(ipcam_ring_buffer_t *rb, const void *in_data, size_t in_bytes);
int ipcam_ring_try_append_meta(ipcam_ring_buffer_t *rb, const void *in_data,
                               size_t in_bytes, const ipcam_frame_meta_t *meta);
/* 最新帧队列写入：满时丢弃最旧帧，保证生产者不会因无人消费而永久停滞。
 * 返回 0=写入且未覆盖旧帧，1=写入成功并覆盖旧帧，-1=关闭或参数错误。 */
int ipcam_ring_try_append_latest_meta(ipcam_ring_buffer_t *rb, const void *in_data,
                                      size_t in_bytes, const ipcam_frame_meta_t *meta);

/*
 * 消费者取一帧：阻塞直到有可用帧或缓冲被关闭且清空。
 * out_frame 由函数填充；rawData/size/seqNo/type 指向 slot 内的内容。
 * 调用者必须在调用 ipcam_ring_release 之前不修改 rawData 所指的 payload。
 * 返回 0=成功，-1=已关闭且无帧。
 */
int ipcam_ring_get(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame);

/*
 * 消费者取最新一帧：等待语义与 ipcam_ring_get 相同，但返回前会释放已经
 * 排队的过期帧，只保留当前最新槽。调用者仍必须与 ipcam_ring_release 成对
 * 使用；该接口只适用于同一 ring 的单消费者模型。
 * 返回 0=成功，-1=已关闭且无帧。
 */
int ipcam_ring_get_latest(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame);

/* 非阻塞取帧：0=成功，1=当前为空，-1=已关闭或参数错误；成功后仍须 release。 */
int ipcam_ring_try_get(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame);

/*
 * 复制当前最新帧而不消费环形缓冲。
 * 直播客户端和 snapshot 共享编码结果时使用该接口，避免多个 reader
 * 互相抢帧；out_data 由调用方提供，容量不足或当前无帧返回 -1。
 * 返回 0 成功，1 表示没有新序号（last_seq 已经是最新），-1 表示错误。
 */
int ipcam_ring_copy_latest(ipcam_ring_buffer_t *rb, void *out_data,
                           size_t out_cap, ipcam_frame_t *out_frame,
                           unsigned long last_seq);

/* 丢弃当前已排队帧；参数变更后用于清理旧配置代次，避免旧帧混入新链路。 */
void ipcam_ring_clear(ipcam_ring_buffer_t *rb);

/* 消费者消费完一帧，slot 进入空闲 */
void ipcam_ring_release(ipcam_ring_buffer_t *rb);

/* 标记缓冲关闭；唤醒所有阻塞线程 */
void ipcam_ring_close(ipcam_ring_buffer_t *rb);

/*
 * 查询缓冲是否已经关闭；状态读取与 count 使用同一把锁，供 healthz
 * 区分“暂时没有帧”和“生产者链路已经停止”。
 */
int ipcam_ring_is_closed(const ipcam_ring_buffer_t *rb);

/* 当前帧数（仅供统计） */
int ipcam_ring_count(ipcam_ring_buffer_t *rb);
size_t ipcam_ring_capacity(const ipcam_ring_buffer_t *rb);
uint64_t ipcam_ring_dropped_count(const ipcam_ring_buffer_t *rb);

/*
 * 工具：给定 frame.rawData，反查它属于哪个 rb 的第几个 slot。
 * 仅在同一进程内有效（指针比较）。返回 -1 表示不在本 rb 内。
 */
int ipcam_ring_slot_index(const ipcam_ring_buffer_t *rb, const void *raw_data);

#endif /* IPCAM_RING_BUFFER_H */
