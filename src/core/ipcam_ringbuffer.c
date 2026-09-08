#include "ipcam_ringbuffer.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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

static void ring_fill_header(ipcam_ring_buffer_t *rb, char *mem,
                             const void *in_data, size_t in_bytes,
                             const ipcam_frame_meta_t *meta)
{
    char *dst = slot_payload(mem);
    ipcam_slot_t *hdr = slot_hdr(mem);

    memcpy(dst, in_data, in_bytes);
    hdr->header.rawData = dst;
    hdr->header.size = in_bytes;
    hdr->header.seqNo = ++rb->seq_counter;
    hdr->header.type = IPCAM_FRAME_TYPE_I;
    if (meta)
        hdr->header.meta = *meta;
    else
        memset(&hdr->header.meta, 0, sizeof(hdr->header.meta));
}

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
    rb->write_idx = 0;
    rb->read_idx = 0;
    rb->count = 0;
    rb->closed = 0;
    rb->seq_counter = 0;
    return rb;
}

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
    free(rb);
}

int ipcam_ring_try_append(ipcam_ring_buffer_t *rb, const void *in_data, size_t in_bytes)
{
    return ipcam_ring_try_append_meta(rb, in_data, in_bytes, NULL);
}

int ipcam_ring_try_append_meta(ipcam_ring_buffer_t *rb, const void *in_data,
                               size_t in_bytes, const ipcam_frame_meta_t *meta)
{
    if (!rb || !in_data) return -1;
    if (in_bytes > rb->slot_bytes) return -1;

    pthread_mutex_lock(&rb->mtx);
    if (rb->closed || rb->count >= rb->depth) {
        pthread_mutex_unlock(&rb->mtx);
        return -1;
    }

    char *mem = rb->slot_mem[rb->write_idx];
    ring_fill_header(rb, mem, in_data, in_bytes, meta);

    rb->write_idx = (rb->write_idx + 1) % rb->depth;
    rb->count++;
    pthread_cond_signal(&rb->cond_not_empty);
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

int ipcam_ring_append(ipcam_ring_buffer_t *rb, const void *in_data, size_t in_bytes)
{
    return ipcam_ring_append_meta(rb, in_data, in_bytes, NULL);
}

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
    ring_fill_header(rb, mem, in_data, in_bytes, meta);

    rb->write_idx = (rb->write_idx + 1) % rb->depth;
    rb->count++;
    pthread_cond_signal(&rb->cond_not_empty);
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

int ipcam_ring_get(ipcam_ring_buffer_t *rb, ipcam_frame_t *out_frame)
{
    if (!rb || !out_frame) return -1;

    pthread_mutex_lock(&rb->mtx);
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
    /* 注意：不移动 read_idx；release 时才推进 */
    pthread_mutex_unlock(&rb->mtx);
    return 0;
}

void ipcam_ring_release(ipcam_ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->mtx);
    rb->read_idx = (rb->read_idx + 1) % rb->depth;
    rb->count--;
    pthread_cond_signal(&rb->cond_not_full);
    pthread_mutex_unlock(&rb->mtx);
}

void ipcam_ring_close(ipcam_ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->mtx);
    rb->closed = 1;
    pthread_cond_broadcast(&rb->cond_not_empty);
    pthread_cond_broadcast(&rb->cond_not_full);
    pthread_mutex_unlock(&rb->mtx);
}

int ipcam_ring_is_closed(ipcam_ring_buffer_t *rb)
{
    if (!rb) return 1;

    /*
     * closed 与 count 都受同一把锁保护；健康检查必须读取一致状态，
     * 否则可能把刚关闭且已清空的 ring 误报成“暂时没有帧”。
     */
    pthread_mutex_lock(&rb->mtx);
    int closed = rb->closed;
    pthread_mutex_unlock(&rb->mtx);
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
