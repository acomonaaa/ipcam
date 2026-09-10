#include "ipcam_ringbuffer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/*
 * 内部 slot 头大小；调用方通过宏 IPCAM_SLOT_PAYLOAD_OFF 跳过它。
 */

static void init_cond_monotonic(pthread_cond_t *cond)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

static ipcam_slot_t *slot_hdr(char *mem) { return (ipcam_slot_t *)mem; }
static char *slot_payload(char *mem)     { return mem + IPCAM_SLOT_PAYLOAD_OFF; }

/* 记录帧产生顺序，消费者不依赖墙上时钟。 */
static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* 根据相对毫秒数生成条件变量所需的 CLOCK_MONOTONIC 绝对截止时间。 */
static int make_deadline(int timeout_ms, struct timespec *deadline)
{
    if (!deadline || timeout_ms < 0) return -1;
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) return -1;
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

/* 分配带帧头的槽位并初始化条件变量；失败时释放已分配资源。 */
ipcam_ring_buffer_t *ipcam_ring_create(int depth, size_t slot_bytes)
{
    if (depth <= 0 || slot_bytes == 0) return NULL;

    ipcam_ring_buffer_t *rb = calloc(1, sizeof(*rb));
    if (!rb) return NULL;

    rb->depth = depth;
    rb->slot_bytes = slot_bytes;
    rb->slot_stride = IPCAM_SLOT_PAYLOAD_OFF + slot_bytes;

    rb->slot_mem = calloc((size_t)depth, sizeof(char *));
    if (!rb->slot_mem) { free(rb); return NULL; }

    for (int i = 0; i < depth; i++) {
        rb->slot_mem[i] = calloc(1, rb->slot_stride);
        if (!rb->slot_mem[i]) {
            for (int j = 0; j < i; j++) free(rb->slot_mem[j]);
            free(rb->slot_mem);
            free(rb);
            return NULL;
        }
        /* 默认 rawData 指向本 slot payload 起点（即便未填充也合法） */
        slot_hdr(rb->slot_mem[i])->header.rawData = slot_payload(rb->slot_mem[i]);
    }

    pthread_mutex_init(&rb->mtx, NULL);
    init_cond_monotonic(&rb->cond_not_empty);
    init_cond_monotonic(&rb->cond_not_full);
    init_cond_monotonic(&rb->cond_updated);
    rb->write_idx = 0;
    rb->read_idx = 0;
    rb->count = 0;
    rb->read_held = 0;
    rb->closed = 0;
    rb->seq_counter = 0;
    rb->dropped_count = 0;
    return rb;
}

/* 销毁环形缓冲；调用者必须先停止所有生产者和消费者线程。 */
void ipcam_ring_destroy(ipcam_ring_buffer_t *rb)
{
    if (!rb) return;
    if (rb->slot_mem) {
        for (int i = 0; i < rb->depth; i++) free(rb->slot_mem[i]);
        free(rb->slot_mem);
    }
    pthread_mutex_destroy(&rb->mtx);
    pthread_cond_destroy(&rb->cond_not_empty);
    pthread_cond_destroy(&rb->cond_not_full);
    pthread_cond_destroy(&rb->cond_updated);
    free(rb);
}

/* 在一次持锁写入中填充公共帧元数据，避免异步消费者拿到悬空指针。 */
static void fill_frame_header(ipcam_slot_t *hdr, char *dst, size_t in_bytes,
                              unsigned long seq, const ipcam_frame_meta_t *meta)
{
    hdr->header.rawData = dst;
    hdr->header.size = in_bytes;
    hdr->header.seqNo = seq;
    hdr->header.type = IPCAM_FRAME_TYPE_I;
    hdr->header.monotonic_ns = meta && meta->monotonic_ns ? meta->monotonic_ns : monotonic_ns();
    hdr->header.width = meta ? meta->width : 0;
    hdr->header.height = meta ? meta->height : 0;
    hdr->header.stride = meta ? meta->stride : 0;
    hdr->header.pixel_format = meta ? meta->pixel_format : 0;
    hdr->header.config_generation = meta ? meta->config_generation : 0;
    hdr->header.quality = meta ? meta->quality : 0;
}

/* 非阻塞写入：满队列计数并丢当前帧，保障采集线程不会被慢消费者拖住。 */
int ipcam_ring_try_append_meta(ipcam_ring_buffer_t *rb, const void *in_data,
                               size_t in_bytes, const ipcam_frame_meta_t *meta)
{
    if (!rb || !in_data) return -1;
    if (in_bytes > rb->slot_bytes) return -1;

    pthread_mutex_lock(&rb->mtx);
    if (rb->closed || rb->count >= rb->depth) {
        if (rb->count >= rb->depth) rb->dropped_count++;
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    char *mem = rb->slot_mem[rb->write_idx];
    char *dst = slot_payload(mem);
    memcpy(dst, in_data, in_bytes);

    ipcam_slot_t *hdr = slot_hdr(mem);
    fill_frame_header(hdr, dst, in_bytes, ++rb->seq_counter, meta);

    rb->write_idx = (rb->write_idx + 1) % rb->depth;
    rb->count++;
    pthread_cond_signal(&rb->cond_not_empty);
    pthread_cond_broadcast(&rb->cond_updated);
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

/* 兼容旧调用方的非阻塞写入；没有元数据时仍自动生成序号和时间戳。 */
int ipcam_ring_try_append(ipcam_ring_buffer_t *rb, const void *in_data, size_t in_bytes)
{
    return ipcam_ring_try_append_meta(rb, in_data, in_bytes, NULL);
}

/* 最新帧写入：满时前移读指针丢旧帧，适用于 HTTP 直播观察者。 */
int ipcam_ring_try_append_latest_meta(ipcam_ring_buffer_t *rb, const void *in_data,
                                      size_t in_bytes, const ipcam_frame_meta_t *meta)
{
    if (!rb || !in_data || in_bytes > rb->slot_bytes) return -1;
    int overwritten = 0;
    pthread_mutex_lock(&rb->mtx);
    if (rb->closed) { pthread_mutex_unlock(&rb->mtx); return -1; }
    if (rb->count >= rb->depth) {
        /* 借用槽仍归消费者所有；满队列时没有可安全覆盖的槽位。 */
        if (rb->read_held) {
            rb->dropped_count++;
            pthread_mutex_unlock(&rb->mtx);
            return -1;
        }
        /* 直播只关心最新画面；主动释放最旧槽，避免 ring 满后永远不再前进。 */
        rb->dropped_count++;
        overwritten = 1;
        rb->read_idx = (rb->read_idx + 1) % rb->depth;
        rb->count--;
    }
    char *mem = rb->slot_mem[rb->write_idx];
    char *dst = slot_payload(mem);
    memcpy(dst, in_data, in_bytes);
    fill_frame_header(slot_hdr(mem), dst, in_bytes, ++rb->seq_counter, meta);
    rb->write_idx = (rb->write_idx + 1) % rb->depth;
    rb->count++;
    pthread_cond_signal(&rb->cond_not_empty);
    pthread_cond_broadcast(&rb->cond_updated);
    pthread_mutex_unlock(&rb->mtx);
    return overwritten;
}

/* 阻塞写入：录像等可靠队列可用，但不能接到采集生产者。 */
int ipcam_ring_append_meta(ipcam_ring_buffer_t *rb, const void *in_data,
                           size_t in_bytes, const ipcam_frame_meta_t *meta)
{
    if (!rb || !in_data) return -1;
    if (in_bytes > rb->slot_bytes) return -1;

    pthread_mutex_lock(&rb->mtx);
    while (rb->count >= rb->depth && !rb->closed) {
        pthread_cond_wait(&rb->cond_not_full, &rb->mtx);
    }
    if (rb->closed) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    char *mem = rb->slot_mem[rb->write_idx];
    char *dst = slot_payload(mem);
    memcpy(dst, in_data, in_bytes);

    ipcam_slot_t *hdr = slot_hdr(mem);
    fill_frame_header(hdr, dst, in_bytes, ++rb->seq_counter, meta);

    rb->write_idx = (rb->write_idx + 1) % rb->depth;
    rb->count++;
    pthread_cond_signal(&rb->cond_not_empty);
    pthread_cond_broadcast(&rb->cond_updated);
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

int ipcam_ring_get(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame)
{
    if (!rb || !out_frame) return -1;

    pthread_mutex_lock(&rb->mtx);
    /* 一个 ring 只有一个消费者；上一次借用未归还时拒绝重复借用。 */
    if (rb->read_held) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    while (rb->count == 0 && !rb->closed) {
        pthread_cond_wait(&rb->cond_not_empty, &rb->mtx);
    }
    if (rb->count == 0 && rb->closed) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    char *mem = rb->slot_mem[rb->read_idx];
    ipcam_slot_t *hdr = slot_hdr(mem);
    *out_frame = hdr->header;
    /* 注意：不移动 read_idx；release 时才推进，并锁住该槽避免生产者覆盖。 */
    rb->read_held = 1;
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

/*
 * 低延迟消费者：编码线程不应按 FIFO 把已经落后的画面逐张压完。
 * 采集生产者仍使用普通非阻塞写入，因而这里释放的只是尚未被消费者
 * 持有的旧槽；返回的最新槽在 release 前不会被本接口再次覆盖。
 */
/*
 * get_latest 的公共实现。timeout_ms=-1 只由旧的阻塞 API 使用；新的定时
 * API 传入非负毫秒，截止时间在首次等待前生成，避免虚假唤醒反复延长等待。
 */
static int ring_get_latest_internal(ipcam_ring_buffer_t *rb,
                                    ipcam_frame_t *out_frame,
                                    unsigned int *stale_count, int timeout_ms)
{
    if (!rb || !out_frame || timeout_ms < -1) return -1;

    pthread_mutex_lock(&rb->mtx);
    if (rb->read_held) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    struct timespec deadline;
    int have_deadline = timeout_ms >= 0 && make_deadline(timeout_ms, &deadline) == 0;
    if (timeout_ms >= 0 && !have_deadline) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    while (rb->count == 0 && !rb->closed) {
        int wait_rc = timeout_ms < 0 ? pthread_cond_wait(&rb->cond_not_empty, &rb->mtx) :
                      pthread_cond_timedwait(&rb->cond_not_empty, &rb->mtx, &deadline);
        if (wait_rc == ETIMEDOUT) {
            pthread_mutex_unlock(&rb->mtx);
            return 1;
        }
        if (wait_rc != 0) {
            pthread_mutex_unlock(&rb->mtx);
            return -1;
        }
    }
    if (rb->count == 0 && rb->closed) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    /* write_idx 指向下一个写槽；其前一个槽就是当前最新帧。 */
    int latest_idx = (rb->write_idx + rb->depth - 1) % rb->depth;
    int stale = rb->count - 1;
    if (stale > 0) {
        /* 这些帧尚未交给消费者，直接归还槽位并累计低延迟丢弃统计。 */
        rb->dropped_count += (uint64_t)stale;
        rb->read_idx = latest_idx;
        rb->count = 1;
        pthread_cond_broadcast(&rb->cond_not_full);
    }

    *out_frame = slot_hdr(rb->slot_mem[latest_idx])->header;
    rb->read_held = 1;
    if (stale_count) *stale_count = (unsigned int)stale;
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

int ipcam_ring_get_latest(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame)
{
    return ring_get_latest_internal(rb, out_frame, NULL, -1);
}

int ipcam_ring_get_latest_ex(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame,
                             unsigned int *stale_count, int timeout_ms)
{
    if (timeout_ms < 0) return -1;
    return ring_get_latest_internal(rb, out_frame, stale_count, timeout_ms);
}

int ipcam_ring_get_timed(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame,
                         int timeout_ms)
{
    if (!rb || !out_frame || timeout_ms < 0) return -1;

    pthread_mutex_lock(&rb->mtx);
    if (rb->read_held) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    struct timespec deadline;
    if (make_deadline(timeout_ms, &deadline) != 0) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    while (rb->count == 0 && !rb->closed) {
        int wait_rc = pthread_cond_timedwait(&rb->cond_not_empty, &rb->mtx, &deadline);
        if (wait_rc == ETIMEDOUT) {
            pthread_mutex_unlock(&rb->mtx);
            return 1;
        }
        if (wait_rc != 0) {
            pthread_mutex_unlock(&rb->mtx);
            return -1;
        }
    }
    if (rb->count == 0 && rb->closed) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    *out_frame = slot_hdr(rb->slot_mem[rb->read_idx])->header;
    rb->read_held = 1;
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

void ipcam_ring_release(ipcam_ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->mtx);
    /* 释放必须与一次成功 ring_get 成对；防御性忽略重复 release，避免计数下溢。 */
    if (rb->read_held && rb->count > 0) {
        rb->read_idx = (rb->read_idx + 1) % rb->depth;
        rb->count--;
        rb->read_held = 0;
        pthread_cond_signal(&rb->cond_not_full);
    }
    pthread_mutex_unlock(&rb->mtx);
}

void ipcam_ring_close(ipcam_ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->mtx);
    rb->closed = 1;
    pthread_cond_broadcast(&rb->cond_not_empty);
    pthread_cond_broadcast(&rb->cond_not_full);
    pthread_cond_broadcast(&rb->cond_updated);
    pthread_mutex_unlock(&rb->mtx);
}

/*
 * closed 与 count 都受 ring mutex 保护；健康检查必须读取一致状态，不能
 * 把已经关闭的输出误报成只是暂时没有新帧。
 */
int ipcam_ring_is_closed(const ipcam_ring_buffer_t *rb)
{
    if (!rb) return 1;
    pthread_mutex_lock((pthread_mutex_t *)&rb->mtx);
    int closed = rb->closed;
    pthread_mutex_unlock((pthread_mutex_t *)&rb->mtx);
    return closed;
}

int ipcam_ring_count(ipcam_ring_buffer_t *rb)
{
    if (!rb) return 0;
    pthread_mutex_lock(&rb->mtx);
    int n = rb->count;
    pthread_mutex_unlock(&rb->mtx);
    return n;
}

int ipcam_ring_slot_index(const ipcam_ring_buffer_t *rb, const void *raw_data)
{
    if (!rb || !raw_data) return -1;
    for (int i = 0; i < rb->depth; i++) {
        const char *mem = rb->slot_mem[i];
        if (raw_data >= (const void *)mem &&
            raw_data <  (const void *)(mem + rb->slot_stride)) {
            return i;
        }
    }
    return -1;
}

/* 返回单槽 payload 上限，供网络/录像线程按真实缓冲分配本地副本。 */
size_t ipcam_ring_capacity(const ipcam_ring_buffer_t *rb)
{
    return rb ? rb->slot_bytes : 0;
}

/* 读取满队列累计丢帧数，统计查询不改变消费位置。 */
uint64_t ipcam_ring_dropped_count(const ipcam_ring_buffer_t *rb)
{
    if (!rb) return 0;
    /* 查询接口保留 const 语义；锁本身不改变环内容，仅需去掉类型限定。 */
    pthread_mutex_lock((pthread_mutex_t *)&rb->mtx);
    uint64_t n = rb->dropped_count;
    pthread_mutex_unlock((pthread_mutex_t *)&rb->mtx);
    return n;
}

/* 兼容旧调用方的阻塞写入；公共帧描述由 ring 内部补齐。 */
int ipcam_ring_append(ipcam_ring_buffer_t *rb, const void *in_data, size_t in_bytes)
{
    return ipcam_ring_append_meta(rb, in_data, in_bytes, NULL);
}

/* 复制最新槽而不移动读指针，多个 HTTP 客户端可并发获得同源帧。 */
int ipcam_ring_copy_latest(ipcam_ring_buffer_t *rb, void *out_data,
                           size_t out_cap, ipcam_frame_t *out_frame,
                           unsigned long last_seq)
{
    if (!rb || !out_data || !out_frame) return -1;

    pthread_mutex_lock(&rb->mtx);
    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    /* write_idx 指向下一个空槽，因此前一个槽就是当前最新帧。 */
    int idx = (rb->write_idx + rb->depth - 1) % rb->depth;
    ipcam_slot_t *hdr = slot_hdr(rb->slot_mem[idx]);
    if (hdr->header.seqNo == last_seq) {
        pthread_mutex_unlock(&rb->mtx);
        return 1;
    }
    if (hdr->header.size > out_cap) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    memcpy(out_data, hdr->header.rawData, hdr->header.size);
    *out_frame = hdr->header;
    out_frame->rawData = out_data;
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

/*
 * 等待编码器发布的新序号再复制，复制动作仍在 ring 锁内完成，保证本地
 * buffer 不会拿到半帧。close 会唤醒等待者，使网络线程可以及时退出。
 */
int ipcam_ring_copy_latest_wait(ipcam_ring_buffer_t *rb, void *out_data,
                                size_t out_cap, ipcam_frame_t *out_frame,
                                unsigned long last_seq, int timeout_ms)
{
    if (!rb || !out_data || !out_frame || timeout_ms < 0) return -1;

    pthread_mutex_lock(&rb->mtx);
    struct timespec deadline;
    if (make_deadline(timeout_ms, &deadline) != 0) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    for (;;) {
        int has_new = 0;
        if (rb->count > 0) {
            int idx = (rb->write_idx + rb->depth - 1) % rb->depth;
            has_new = slot_hdr(rb->slot_mem[idx])->header.seqNo != last_seq;
        }
        if (has_new) break;
        if (rb->closed) {
            pthread_mutex_unlock(&rb->mtx);
            return -1;
        }

        int wait_rc = pthread_cond_timedwait(&rb->cond_updated, &rb->mtx, &deadline);
        if (wait_rc == ETIMEDOUT) {
            pthread_mutex_unlock(&rb->mtx);
            return 1;
        }
        if (wait_rc != 0) {
            pthread_mutex_unlock(&rb->mtx);
            return -1;
        }
    }

    int idx = (rb->write_idx + rb->depth - 1) % rb->depth;
    ipcam_slot_t *hdr = slot_hdr(rb->slot_mem[idx]);
    if (hdr->header.size > out_cap) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    memcpy(out_data, hdr->header.rawData, hdr->header.size);
    *out_frame = hdr->header;
    out_frame->rawData = out_data;
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

/* 非阻塞取帧供录像线程轮询 stop_requested；避免无新帧时永久睡在条件变量上。 */
int ipcam_ring_try_get(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame)
{
    if (!rb || !out_frame) return -1;
    pthread_mutex_lock(&rb->mtx);
    if (rb->read_held) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }
    if (rb->count == 0) {
        int closed = rb->closed;
        pthread_mutex_unlock(&rb->mtx);
        return closed ? -1 : 1;
    }
    char *mem = rb->slot_mem[rb->read_idx];
    *out_frame = slot_hdr(mem)->header;
    rb->read_held = 1;
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

/* 参数代次切换时清空旧帧，唤醒可能等待空槽的生产者。 */
void ipcam_ring_clear(ipcam_ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->mtx);
    if (rb->read_held) {
        /* 清空配置旧帧时不能使调用者手中的 rawData 失效，保留借用槽，
         * 其余排队帧直接丢弃，并把下一次写入定位到借用槽之后。 */
        rb->write_idx = (rb->read_idx + 1) % rb->depth;
        rb->count = 1;
    } else {
        rb->read_idx = rb->write_idx;
        rb->count = 0;
    }
    pthread_cond_broadcast(&rb->cond_not_full);
    pthread_cond_broadcast(&rb->cond_updated);
    pthread_mutex_unlock(&rb->mtx);
}
