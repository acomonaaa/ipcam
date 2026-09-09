#define _GNU_SOURCE

#include "ipcam_record.h"
#include "ipcam_config.h"
#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define AVI_INDEX_GROW 256

/* 本服务从编码广播队列单独取帧，负责 SD 挂载校验、AVI 收尾和照片落盘。 */

typedef struct avi_index_entry_s {
    uint32_t offset;
    uint32_t size;
} avi_index_entry_t;

typedef struct avi_segment_s {
    FILE *fp;
    int fd;
    char temp_path[256];
    char final_path[256];
    off_t riff_size_pos;
    off_t hdrl_size_pos;
    off_t strl_size_pos;
    off_t avih_total_frames_pos;
    off_t strh_length_pos;
    off_t movi_size_pos;
    off_t movi_data_start;
    uint64_t start_ns;
    uint64_t frame_count;
    uint64_t bytes_written;
    uint64_t container_bytes; /* 含 AVI 头、chunk 头和 padding 的实际累计大小 */
    uint64_t drop_base;
    unsigned char *last_jpeg; /* 用于固定时间轴的缺帧重复，不借用 ring 槽内存 */
    size_t last_jpeg_size;
    size_t last_jpeg_cap;
    uint64_t next_slot_ns;
    avi_index_entry_t *index;
    size_t index_count;
    size_t index_cap;
} avi_segment_t;

/* 预估一个 JPEG chunk、idx1 和收尾余量是否仍能落在 32 位 AVI 尺寸上限内。 */
static int avi_payload_fits(const avi_segment_t *seg, size_t size)
{
    if (!seg || size > UINT32_MAX) return 0;
    uint64_t chunk_bytes = 8ULL + (uint64_t)size + (size & 1U);
    uint64_t projected = seg->container_bytes + chunk_bytes +
                         8ULL + ((uint64_t)seg->index_count + 1ULL) * 16ULL + 4096ULL;
    return projected <= IPCAM_RECORD_MAX_SEGMENT_BYTES;
}

/* 读取单调时间，录像分段和固定时间轴不能受墙上时钟校准影响。 */
static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* 录像线程与拍照/停止调用方共享该标志；用 GCC 4.9 可用的原子内建，
 * 不把普通 int 的无锁并发读写留给未定义行为。 */
static int record_is_shutdown(ipcam_record_ctx_t *ctx)
{
    return ctx && __sync_fetch_and_add(&ctx->shutdown_requested, 0) != 0;
}

/* 写入固定长度字节块；fwrite 短写统一转换为失败。 */
static int write_bytes(FILE *fp, const void *p, size_t n)
{
    return fwrite(p, 1, n, fp) == n ? 0 : -1;
}

/* 普通 write 允许短写；照片落盘必须循环直到完整 JPEG 写完。 */
static int write_all_fd(int fd, const void *data, size_t size)
{
    const unsigned char *p = data;
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, p + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* AVI 所有整数均按 little-endian 写出，避免 ARM 主机字节序假设。 */
static int write_u32le(FILE *fp, uint32_t value)
{
    unsigned char b[4] = {
        (unsigned char)(value & 0xff),
        (unsigned char)((value >> 8) & 0xff),
        (unsigned char)((value >> 16) & 0xff),
        (unsigned char)((value >> 24) & 0xff)
    };
    return write_bytes(fp, b, sizeof(b));
}

/* 写入四字符码；调用方保证 id 至少包含 4 个字节。 */
static int write_fourcc(FILE *fp, const char id[4])
{
    return write_bytes(fp, id, 4);
}

/* 在文件收尾阶段回填 RIFF/AVI 的未知长度字段，并恢复写指针。 */
static int patch_u32(FILE *fp, off_t pos, uint32_t value)
{
    off_t end = ftello(fp);
    if (end < 0 || fseeko(fp, pos, SEEK_SET) != 0 || write_u32le(fp, value) != 0)
        return -1;
    return fseeko(fp, end, SEEK_SET);
}

/* 在状态锁内记录可供 HTTP/GUI 查询的持久错误原因。 */
static void status_error(ipcam_record_ctx_t *ctx, const char *message)
{
    pthread_mutex_lock(&ctx->mtx);
    ctx->status.state = IPCAM_RECORD_ERROR;
    snprintf(ctx->status.last_error, sizeof(ctx->status.last_error), "%s", message);
    pthread_mutex_unlock(&ctx->mtx);
}

/*
 * 只探测目录是否位于独立挂载设备，并读取该设备空间；挂载判断与录像、
 * 拍照、HTTP 状态共用，避免状态接口把根文件系统误报成 SD 卡空间。
 */
static int storage_probe(const char *root, int *mounted,
                         uint64_t *available_bytes, char *error, size_t error_sz)
{
    struct stat st_root, st_parent;
    struct statvfs vfs;
    char parent[256];
    if (mounted) *mounted = 0;
    if (available_bytes) *available_bytes = 0;
    if (!root || !*root) {
        snprintf(error, error_sz, "存储路径为空");
        return -1;
    }
    int parent_len = snprintf(parent, sizeof(parent), "%s/..", root);
    if (parent_len < 0 || (size_t)parent_len >= sizeof(parent)) {
        snprintf(error, error_sz, "存储路径过长");
        return -1;
    }
    if (stat(root, &st_root) != 0 || !S_ISDIR(st_root.st_mode)) {
        snprintf(error, error_sz, "存储目录不可用: %s", root);
        return -1;
    }
    /* 设备号不同才说明 root 位于独立挂载上，避免 SD 未挂载时写入根分区。 */
    if (stat(parent, &st_parent) != 0 || st_root.st_dev == st_parent.st_dev) {
        snprintf(error, error_sz, "存储目录未挂载: %s", root);
        return -1;
    }
    if (mounted) *mounted = 1;
    if (statvfs(root, &vfs) != 0) {
        snprintf(error, error_sz, "无法读取存储空间: %s", root);
        return -1;
    }
    if (available_bytes)
        *available_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    return 0;
}

/* 同时检查目录身份和剩余空间，防止 SD 掉挂后落到根文件系统。 */
static int storage_check(const char *root, char *error, size_t error_sz)
{
    uint64_t available = 0;
    if (storage_probe(root, NULL, &available, error, error_sz) != 0)
        return -1;
    if (available <= IPCAM_RECORD_RESERVE_BYTES) {
        snprintf(error, error_sz, "存储空间不足");
        return -1;
    }
    return 0;
}

/* 写入前预留当前 JPEG、索引和 AVI 收尾空间；低空间时主动终止本段，
 * 防止文件尾部 idx1 被截断后仍改名为看似完整的 .avi。 */
static int storage_has_room(const char *root, uint64_t extra_bytes)
{
    uint64_t available = 0;
    char ignored_error[1];
    if (storage_probe(root, NULL, &available, ignored_error,
                      sizeof(ignored_error)) != 0) return 0;
    return available > IPCAM_RECORD_RESERVE_BYTES + extra_bytes;
}

/* C99 没有 write_u16le 宏；单独定义以保持 AVI 字段的明确小端布局。 */
/* 写入 AVI 头中的 16 位 little-endian 字段。 */
static int write_u16le(FILE *fp, uint16_t value)
{
    unsigned char b[2] = {(unsigned char)(value & 0xff), (unsigned char)(value >> 8)};
    return write_bytes(fp, b, sizeof(b));
}

/* 写入可被常见播放器识别的单视频流 MJPEG AVI 头，并记录回填位置。 */
static int avi_write_header_full(avi_segment_t *seg, int width, int height, int fps)
{
    FILE *fp = seg->fp;
    uint32_t usec = fps > 0 ? (uint32_t)(1000000 / fps) : 66666;
    off_t hdrl_start, strl_start;
    if (write_fourcc(fp, "RIFF")) return -1;
    seg->riff_size_pos = ftello(fp); if (seg->riff_size_pos < 0 || write_u32le(fp, 0)) return -1;
    if (write_fourcc(fp, "AVI ") || write_fourcc(fp, "LIST")) return -1;
    seg->hdrl_size_pos = ftello(fp); if (seg->hdrl_size_pos < 0 || write_u32le(fp, 0)) return -1;
    hdrl_start = ftello(fp);
    if (write_fourcc(fp, "avih") || write_u32le(fp, 56) ||
        write_u32le(fp, usec) || write_u32le(fp, 0) || write_u32le(fp, 0) ||
        write_u32le(fp, 0x10)) return -1;
    seg->avih_total_frames_pos = ftello(fp); if (seg->avih_total_frames_pos < 0 || write_u32le(fp, 0)) return -1;
    if (write_u32le(fp, 0) || write_u32le(fp, 1) || write_u32le(fp, 0) ||
        write_u32le(fp, 0) || write_u32le(fp, (uint32_t)width) || write_u32le(fp, (uint32_t)height) ||
        write_u32le(fp, 0) || write_u32le(fp, 0) || write_u32le(fp, 0) || write_u32le(fp, 0)) return -1;

    if (write_fourcc(fp, "LIST")) return -1;
    seg->strl_size_pos = ftello(fp); if (seg->strl_size_pos < 0 || write_u32le(fp, 0)) return -1;
    strl_start = ftello(fp);
    if (write_fourcc(fp, "strh") || write_u32le(fp, 56) || write_fourcc(fp, "vids") ||
        write_fourcc(fp, "MJPG") || write_u32le(fp, 0) || write_u16le(fp, 0) || write_u16le(fp, 0) ||
        write_u32le(fp, 0) || write_u32le(fp, 1) || write_u32le(fp, (uint32_t)fps) || write_u32le(fp, 0)) return -1;
    seg->strh_length_pos = ftello(fp); if (seg->strh_length_pos < 0 || write_u32le(fp, 0)) return -1;
    if (write_u32le(fp, 0) || write_u32le(fp, 0xffffffffU) || write_u32le(fp, 0) ||
        write_u16le(fp, 0) || write_u16le(fp, 0) || write_u16le(fp, (uint16_t)width) ||
        write_u16le(fp, (uint16_t)height)) return -1;
    if (write_fourcc(fp, "strf") || write_u32le(fp, 40) || write_u32le(fp, 40) ||
        write_u32le(fp, (uint32_t)width) || write_u32le(fp, (uint32_t)height) ||
        write_u16le(fp, 1) || write_u16le(fp, 24) || write_fourcc(fp, "MJPG") ||
        write_u32le(fp, 0) || write_u32le(fp, 0) || write_u32le(fp, 0) || write_u32le(fp, 0) || write_u32le(fp, 0)) return -1;
    if (patch_u32(fp, seg->strl_size_pos, (uint32_t)(ftello(fp) - strl_start))) return -1;
    if (patch_u32(fp, seg->hdrl_size_pos, (uint32_t)(ftello(fp) - hdrl_start))) return -1;
    if (write_fourcc(fp, "LIST")) return -1;
    seg->movi_size_pos = ftello(fp); if (seg->movi_size_pos < 0 || write_u32le(fp, 0) || write_fourcc(fp, "movi")) return -1;
    seg->movi_data_start = ftello(fp);
    if (seg->movi_data_start < 0) return -1;
    /* 从文件头起计数，后续限制才能覆盖头部和每个 chunk 的容器开销。 */
    seg->container_bytes = (uint64_t)seg->movi_data_start;
    return 0;
}

/* avi_close 在段切换辅助函数之后实现，这里先声明以保持 C99 原型可见。 */
static int avi_close(ipcam_record_ctx_t *ctx, avi_segment_t *seg);

/* 检查存储后排他创建临时分段；只有收尾成功才会改成正式扩展名。 */
static int avi_open_segment(ipcam_record_ctx_t *ctx, avi_segment_t **out)
{
    char error[128];
    if (storage_check(ctx->storage_root, error, sizeof(error)) != 0) {
        status_error(ctx, error);
        return -1;
    }
    char dir[256];
    int dir_len = snprintf(dir, sizeof(dir), "%s/ipcam-recordings", ctx->storage_root);
    if (dir_len < 0 || (size_t)dir_len >= sizeof(dir)) {
        status_error(ctx, "录像目录路径过长");
        return -1;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        status_error(ctx, "创建录像目录失败");
        return -1;
    }
    avi_segment_t *seg = calloc(1, sizeof(*seg));
    if (!seg) return -1;
    uint32_t no;
    pthread_mutex_lock(&ctx->mtx);
    no = ctx->status.segment_no + 1;
    ctx->status.segment_no = no;
    pthread_mutex_unlock(&ctx->mtx);
    int temp_len = snprintf(seg->temp_path, sizeof(seg->temp_path),
                            "%s/%lu-%03u.avi.part", dir, ctx->session_id, no);
    int final_len = snprintf(seg->final_path, sizeof(seg->final_path),
                             "%s/%lu-%03u.avi", dir, ctx->session_id, no);
    if (temp_len < 0 || (size_t)temp_len >= sizeof(seg->temp_path) ||
        final_len < 0 || (size_t)final_len >= sizeof(seg->final_path)) {
        free(seg);
        status_error(ctx, "录像文件路径过长");
        return -1;
    }
    /* O_EXCL 防止异常重启后复用 session/序号时覆盖旧文件。 */
    seg->fd = open(seg->temp_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (seg->fd < 0) { free(seg); status_error(ctx, "创建录像文件失败"); return -1; }
    seg->fp = fdopen(seg->fd, "wb+");
    if (!seg->fp) {
        close(seg->fd); unlink(seg->temp_path); free(seg);
        status_error(ctx, "创建录像流失败");
        return -1;
    }
    if (avi_write_header_full(seg, ctx->width, ctx->height, ctx->fps) != 0) {
        fclose(seg->fp); unlink(seg->temp_path); free(seg); status_error(ctx, "写入 AVI 头失败"); return -1;
    }
    pthread_mutex_lock(&ctx->mtx);
    snprintf(ctx->status.current_file, sizeof(ctx->status.current_file), "%s", seg->final_path);
    pthread_mutex_unlock(&ctx->mtx);
    *out = seg;
    seg->drop_base = ipcam_ring_dropped_count(ctx->jpeg_rb);
    return 0;
}

/* 因容器大小或五分钟上限切换段；新段从下一张有效帧重新建立时间轴。 */
static int avi_rotate_segment(ipcam_record_ctx_t *ctx, avi_segment_t **seg)
{
    if (!ctx || !seg || !*seg) return -1;
    if (avi_close(ctx, *seg) != 0) {
        *seg = NULL;
        status_error(ctx, "录像分段收尾失败");
        return -1;
    }
    *seg = NULL;
    if (avi_open_segment(ctx, seg) != 0) return -1;
    pthread_mutex_lock(&ctx->mtx);
    ctx->status.state = IPCAM_RECORD_RECORDING;
    pthread_mutex_unlock(&ctx->mtx);
    return 0;
}

/* 只负责写一个 movi 里的 JPEG chunk；调用者决定是否更新时间轴缓存。 */
static int avi_append_payload(avi_segment_t *seg, const void *data, size_t size)
{
    /* AVI/RIFF 的单个 chunk 和总长度字段均为 32 位，接近上限必须切段。 */
    if (!avi_payload_fits(seg, size)) return -1;
    if (seg->index_count == seg->index_cap) {
        size_t cap = seg->index_cap + AVI_INDEX_GROW;
        avi_index_entry_t *p = realloc(seg->index, cap * sizeof(*p));
        if (!p) return -1;
        seg->index = p; seg->index_cap = cap;
    }
    off_t pos = ftello(seg->fp);
    if (pos < 0 || write_fourcc(seg->fp, "00dc") ||
        write_u32le(seg->fp, (uint32_t)size) || write_bytes(seg->fp, data, size)) return -1;
    if (size & 1) { if (write_bytes(seg->fp, "\0", 1)) return -1; }
    seg->index[seg->index_count].offset = (uint32_t)(pos - seg->movi_data_start);
    seg->index[seg->index_count].size = (uint32_t)size;
    seg->index_count++;
    seg->frame_count++;
    seg->bytes_written += size;
    seg->container_bytes += 8ULL + (uint64_t)size + (size & 1U);
    return 0;
}

/*
 * 写入真实帧并保留一份副本。副本必须独立于 ring 槽，因为写入完成后
 * 采集/编码线程可能立即复用该槽，录像线程不能把悬空指针留给重复帧。
 */
static int avi_append(avi_segment_t *seg, const ipcam_frame_t *frame)
{
    if (!frame || !frame->rawData || avi_append_payload(seg, frame->rawData, frame->size) != 0)
        return -1;
    if (frame->size > seg->last_jpeg_cap) {
        unsigned char *p = realloc(seg->last_jpeg, frame->size);
        if (!p) return -1;
        seg->last_jpeg = p;
        seg->last_jpeg_cap = frame->size;
    }
    memcpy(seg->last_jpeg, frame->rawData, frame->size);
    seg->last_jpeg_size = frame->size;
    return 0;
}

/* 将上一张 JPEG 补写到当前时间槽，保持 AVI 播放时长与采集时间一致。 */
static int avi_append_repeated(avi_segment_t *seg)
{
    if (!seg || !seg->last_jpeg || seg->last_jpeg_size == 0) return -1;
    return avi_append_payload(seg, seg->last_jpeg, seg->last_jpeg_size);
}

/* 记录已成功写入的帧和字节；与状态快照共用 mtx，主循环可随时读取。 */
static void record_add_metrics(ipcam_record_ctx_t *ctx, uint64_t frames,
                               uint64_t bytes)
{
    pthread_mutex_lock(&ctx->mtx);
    ctx->frames_written_total += frames;
    ctx->bytes_written_total += bytes;
    pthread_mutex_unlock(&ctx->mtx);
}

/*
 * 维护一份独立的最新 JPEG 快照。录像线程会消费 aux ring，若直接从
 * ring 取图，空闲录像时槽位会被释放，拍照命令就会偶发“无帧”；缓存把
 * 拍照语义与录像消费解耦，大小受 JPEG ring 槽上限约束。
 */
static void record_update_latest(ipcam_record_ctx_t *ctx, const ipcam_frame_t *frame)
{
    if (!ctx || !frame || !frame->rawData || frame->size == 0) return;
    pthread_mutex_lock(&ctx->latest_mtx);
    if (frame->size > ctx->latest_cap) {
        unsigned char *p = realloc(ctx->latest_jpeg, frame->size);
        if (!p) {
            pthread_mutex_unlock(&ctx->latest_mtx);
            MLOGW("photo latest cache realloc %zu failed\n", frame->size);
            return;
        }
        ctx->latest_jpeg = p;
        ctx->latest_cap = frame->size;
    }
    memcpy(ctx->latest_jpeg, frame->rawData, frame->size);
    ctx->latest_size = frame->size;
    ctx->latest_seq = frame->seqNo;
    pthread_cond_broadcast(&ctx->latest_cond);
    pthread_mutex_unlock(&ctx->latest_mtx);
}

/* 回填索引、同步并原子改名；失败只删除 .part，不影响已完成分段。 */
static int avi_close(ipcam_record_ctx_t *ctx, avi_segment_t *seg)
{
    if (!seg) return 0;
    FILE *fp = seg->fp;
    off_t idx_start = ftello(fp);
    if (idx_start < 0 || write_fourcc(fp, "idx1") || write_u32le(fp, (uint32_t)(seg->index_count * 16))) goto fail;
    for (size_t i = 0; i < seg->index_count; i++) {
        if (write_fourcc(fp, "00dc") || write_u32le(fp, 0x10) ||
            write_u32le(fp, seg->index[i].offset) || write_u32le(fp, seg->index[i].size)) goto fail;
    }
    off_t end = ftello(fp);
    if (end < 0 || patch_u32(fp, seg->movi_size_pos, (uint32_t)(idx_start - (seg->movi_size_pos + 4))) ||
        patch_u32(fp, seg->avih_total_frames_pos, (uint32_t)seg->frame_count) ||
        patch_u32(fp, seg->strh_length_pos, (uint32_t)seg->frame_count) ||
        patch_u32(fp, seg->riff_size_pos, (uint32_t)(end - 8))) goto fail;
    /* 收尾失败也必须关闭 fd，避免拔卡/重复启停时泄漏文件描述符。 */
    if (fflush(fp) != 0 || fsync(seg->fd) != 0) goto fail;
    if (fclose(fp) != 0) { fp = NULL; goto fail_no_close; }
    fp = NULL;
    if (access(seg->final_path, F_OK) == 0 || rename(seg->temp_path, seg->final_path) != 0)
        goto fail_no_close;
    pthread_mutex_lock(&ctx->mtx);
    ctx->status.frame_count += seg->frame_count;
    ctx->status.bytes_written += seg->bytes_written;
    pthread_mutex_unlock(&ctx->mtx);
    free(seg->last_jpeg);
    free(seg->index); free(seg);
    return 0;
fail:
    if (fp) fclose(fp);
fail_no_close:
    unlink(seg->temp_path);
    free(seg->last_jpeg);
    free(seg->index); free(seg);
    return -1;
}

/* 独立录像消费者：队列溢出即报错收尾，不能反压采集/直播。 */
static void *record_thread(void *arg)
{
    ipcam_record_ctx_t *ctx = arg;
    ipcam_frame_t frame;
    avi_segment_t *seg = NULL;
    while (*ctx->running && !record_is_shutdown(ctx)) {
        int get_rc = ipcam_ring_try_get(ctx->jpeg_rb, &frame);
        if (get_rc == 1) {
            /* 非阻塞轮询让 stop_requested 在无新帧时也能及时收尾；
             * 录像队列仍由编码线程非阻塞写入，不会因这里的 sleep 反压采集。 */
            pthread_mutex_lock(&ctx->mtx);
            int stop_without_frame = ctx->stop_requested;
            int state_without_frame = ctx->status.state;
            if (stop_without_frame && state_without_frame == IPCAM_RECORD_STARTING) {
                /* 尚未收到首帧时没有 AVI 文件，直接完成 STARTING→IDLE。 */
                ctx->stop_requested = 0;
                ctx->status.state = IPCAM_RECORD_IDLE;
                ctx->status.current_file[0] = '\0';
            } else if (stop_without_frame && state_without_frame == IPCAM_RECORD_RECORDING) {
                ctx->status.state = IPCAM_RECORD_STOPPING;
                if (!seg) {
                    /* 理论上 RECORDING 必有活动段；防御异常状态避免停止请求反复挂起。 */
                    ctx->stop_requested = 0;
                    ctx->status.state = IPCAM_RECORD_IDLE;
                    ctx->status.current_file[0] = '\0';
                }
            }
            pthread_mutex_unlock(&ctx->mtx);
            if (stop_without_frame && state_without_frame == IPCAM_RECORD_STARTING)
                continue;
            if (stop_without_frame && state_without_frame == IPCAM_RECORD_RECORDING && seg) {
                if (avi_close(ctx, seg) != 0) status_error(ctx, "录像收尾失败");
                seg = NULL;
                pthread_mutex_lock(&ctx->mtx);
                ctx->stop_requested = 0;
                if (ctx->status.state != IPCAM_RECORD_ERROR)
                    ctx->status.state = IPCAM_RECORD_IDLE;
                ctx->status.current_file[0] = '\0';
                pthread_mutex_unlock(&ctx->mtx);
                continue;
            }
            usleep(10 * 1000);
            continue;
        }
        if (get_rc != 0) break;
        record_update_latest(ctx, &frame);
        pthread_mutex_lock(&ctx->mtx);
        int state = ctx->status.state;
        int stop = ctx->stop_requested;
        pthread_mutex_unlock(&ctx->mtx);
        int started_now = 0;

        if (state == IPCAM_RECORD_STARTING) {
            if (avi_open_segment(ctx, &seg) == 0) {
                /* STARTING 与首个有效帧绑定：文件头打开成功后才对外报告录像。 */
                pthread_mutex_lock(&ctx->mtx);
                ctx->status.state = IPCAM_RECORD_RECORDING;
                int stop_after_start = ctx->stop_requested;
                ctx->stop_requested = 0;
                pthread_mutex_unlock(&ctx->mtx);
                if (stop_after_start) {
                    pthread_mutex_lock(&ctx->mtx);
                    ctx->status.state = IPCAM_RECORD_STOPPING;
                    pthread_mutex_unlock(&ctx->mtx);
                    if (avi_close(ctx, seg) != 0) status_error(ctx, "录像收尾失败");
                    seg = NULL;
                    pthread_mutex_lock(&ctx->mtx);
                    if (ctx->status.state != IPCAM_RECORD_ERROR)
                        ctx->status.state = IPCAM_RECORD_IDLE;
                    ctx->status.current_file[0] = '\0';
                    pthread_mutex_unlock(&ctx->mtx);
                } else started_now = 1;
            }
        }
        /* 若恰好在 STARTING 帧上收到 stop，以上分支已经收尾，本帧不再写入。 */
        if (seg && (state == IPCAM_RECORD_RECORDING || started_now)) {
            if (ipcam_ring_dropped_count(ctx->jpeg_rb) != seg->drop_base) {
                status_error(ctx, "录像帧队列溢出");
                avi_close(ctx, seg);
                seg = NULL;
                ipcam_ring_release(ctx->jpeg_rb);
                continue;
            }
            if (stop) {
                pthread_mutex_lock(&ctx->mtx); ctx->status.state = IPCAM_RECORD_STOPPING; pthread_mutex_unlock(&ctx->mtx);
                if (avi_close(ctx, seg) != 0) status_error(ctx, "录像收尾失败");
                seg = NULL;
                pthread_mutex_lock(&ctx->mtx); if (ctx->status.state != IPCAM_RECORD_ERROR) ctx->status.state = IPCAM_RECORD_IDLE; ctx->status.current_file[0] = '\0'; pthread_mutex_unlock(&ctx->mtx);
            } else {
                uint64_t frame_ns = frame.monotonic_ns ? frame.monotonic_ns : now_ns();
                uint64_t interval_ns = ctx->fps > 0 ? 1000000000ULL / (uint64_t)ctx->fps : 66666666ULL;
                int write_failed = 0;
                pthread_mutex_lock(&ctx->mtx);
                if (ctx->recording_start_ns == 0) ctx->recording_start_ns = frame_ns;
                uint64_t recording_start_ns = ctx->recording_start_ns;
                pthread_mutex_unlock(&ctx->mtx);
                /* 在处理跨越五分钟边界的帧之前先收尾旧段，避免先补帧再
                 * 轮转导致单段实际时间超过上限；当前帧成为新段首帧。 */
                if (seg->start_ns > 0 &&
                    frame_ns >= seg->start_ns +
                    (uint64_t)IPCAM_RECORD_SEGMENT_SECONDS * 1000000000ULL) {
                    if (avi_rotate_segment(ctx, &seg) != 0) write_failed = 1;
                }
                /* 当前 JPEG 放不下时先完整收尾旧段，再让本帧成为新段首帧；
                 * 这样 3 GiB 限制会提前切段而不是把错误文件改成正式名。 */
                if (!write_failed && seg && seg->frame_count > 0 &&
                    !avi_payload_fits(seg, frame.size)) {
                    if (avi_rotate_segment(ctx, &seg) != 0) write_failed = 1;
                }
                if (!write_failed && seg && seg->start_ns == 0) {
                    seg->start_ns = frame_ns;
                    seg->next_slot_ns = frame_ns + interval_ns;
                } else if (!write_failed && seg) {
                    /*
                     * 输入帧因调度或摄像头短暂无帧而产生时间空洞时，按固定
                     * fps 复制上一张 JPEG。这样播放器不会把缺帧误解释为
                     * 加速播放；重复数量单独计入状态，便于验收定位瓶颈。
                     */
                    unsigned int guard = 0;
                    while (seg->last_jpeg_size > 0 && seg->next_slot_ns > 0 &&
                           frame_ns > seg->next_slot_ns && guard++ < 600) {
                        uint64_t reserve = 4096ULL +
                            ((uint64_t)seg->index_count + 1ULL) * 16ULL +
                            8ULL + seg->last_jpeg_size + (seg->last_jpeg_size & 1U);
                        if (!avi_payload_fits(seg, seg->last_jpeg_size)) {
                            /* 新段不重复上一段的尾帧，当前真实帧随后作为首帧写入。 */
                            if (avi_rotate_segment(ctx, &seg) != 0) write_failed = 1;
                            break;
                        }
                        if (!storage_has_room(ctx->storage_root, reserve)) {
                            write_failed = 1;
                            break;
                        }
                        if (avi_append_repeated(seg) != 0) {
                            write_failed = 1;
                            break;
                        }
                        record_add_metrics(ctx, 1, seg->last_jpeg_size);
                        pthread_mutex_lock(&ctx->mtx);
                        ctx->status.repeated_frames++;
                        pthread_mutex_unlock(&ctx->mtx);
                        seg->next_slot_ns += interval_ns;
                    }
                }
                if (!write_failed && seg) {
                    uint64_t reserve = 4096ULL +
                        ((uint64_t)seg->index_count + 1ULL) * 16ULL +
                        8ULL + frame.size + (frame.size & 1U);
                    if (!storage_has_room(ctx->storage_root, reserve) ||
                        avi_append(seg, &frame) != 0)
                        write_failed = 1;
                    else
                        record_add_metrics(ctx, 1, frame.size);
                }
                if (write_failed) {
                    status_error(ctx, "录像写入失败");
                    avi_close(ctx, seg);
                    seg = NULL;
                } else {
                    seg->next_slot_ns = frame_ns + interval_ns;
                    pthread_mutex_lock(&ctx->mtx);
                    ctx->status.elapsed_ms = frame_ns > recording_start_ns ?
                        (frame_ns - recording_start_ns) / 1000000ULL : 0;
                    pthread_mutex_unlock(&ctx->mtx);
                }
            }
        }
        ipcam_ring_release(ctx->jpeg_rb);
    }
    if (seg) { if (avi_close(ctx, seg) != 0) status_error(ctx, "退出时录像收尾失败"); }
    return NULL;
}

/* 创建录像线程但保持 IDLE，实际文件从下一张有效 JPEG 开始。 */
int ipcam_record_start(ipcam_record_ctx_t *ctx, ipcam_ring_buffer_t *jpeg_rb,
                       volatile sig_atomic_t *running, const char *storage_root,
                       int width, int height, int fps)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->jpeg_rb = jpeg_rb; ctx->running = running;
    ctx->width = width > 0 ? width : IPCAM_CAPTURE_WIDTH;
    ctx->height = height > 0 ? height : IPCAM_CAPTURE_HEIGHT;
    ctx->fps = fps > 0 ? fps : IPCAM_TARGET_FPS;
    snprintf(ctx->storage_root, sizeof(ctx->storage_root), "%s", storage_root && *storage_root ? storage_root : IPCAM_STORAGE_ROOT);
    ctx->session_id = (unsigned long)time(NULL) ^ (unsigned long)getpid();
    pthread_mutex_init(&ctx->mtx, NULL);
    pthread_mutex_init(&ctx->latest_mtx, NULL);
    pthread_cond_init(&ctx->latest_cond, NULL);
    ctx->status.state = IPCAM_RECORD_IDLE;
    if (pthread_create(&ctx->thread, NULL, record_thread, ctx) != 0) {
        pthread_cond_destroy(&ctx->latest_cond);
        pthread_mutex_destroy(&ctx->latest_mtx);
        pthread_mutex_destroy(&ctx->mtx);
        return -1;
    }
    return 0;
}

/* 受理开始请求；状态切换和文件创建由录像线程串行完成。 */
int ipcam_record_request_start(ipcam_record_ctx_t *ctx)
{
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->mtx);
    if (ctx->status.state == IPCAM_RECORD_RECORDING || ctx->status.state == IPCAM_RECORD_STARTING) { pthread_mutex_unlock(&ctx->mtx); return -1; }
    ctx->status.state = IPCAM_RECORD_STARTING;
    ctx->status.last_error[0] = '\0';
    ctx->status.elapsed_ms = 0;
    ctx->recording_start_ns = 0;
    ctx->stop_requested = 0;
    pthread_mutex_unlock(&ctx->mtx); return 0;
}

/* 受理停止请求；线程会在当前队列帧边界完成 AVI 索引和同步。 */
int ipcam_record_request_stop(ipcam_record_ctx_t *ctx)
{
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->mtx);
    if (ctx->status.state != IPCAM_RECORD_RECORDING && ctx->status.state != IPCAM_RECORD_STARTING) { pthread_mutex_unlock(&ctx->mtx); return -1; }
    ctx->stop_requested = 1;
    pthread_mutex_unlock(&ctx->mtx); return 0;
}

/* 复制状态快照，调用方不持有录像锁即可格式化 JSON。 */
void ipcam_record_get_status(ipcam_record_ctx_t *ctx, ipcam_record_status_t *out)
{
    if (!ctx || !out) return;
    pthread_mutex_lock(&ctx->mtx); *out = ctx->status; pthread_mutex_unlock(&ctx->mtx);
}

/*
 * 状态接口只报告挂载后的 SD 空间；这里不要求满足录像预留阈值，
 * 让 GUI 能区分“已挂载但空间不足”和“目录落在根文件系统”。
 */
int ipcam_record_get_storage_status(ipcam_record_ctx_t *ctx,
                                    int *mounted, uint64_t *available_bytes)
{
    if (!ctx) return -1;
    char error[128];
    return storage_probe(ctx->storage_root, mounted, available_bytes,
                         error, sizeof(error));
}

/* 复制含活动段的写入统计；文件收尾失败时仍能看到已写入的工作量。 */
void ipcam_record_get_metrics(ipcam_record_ctx_t *ctx, uint64_t *frames,
                              uint64_t *bytes)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->mtx);
    if (frames) *frames = ctx->frames_written_total;
    if (bytes) *bytes = ctx->bytes_written_total;
    pthread_mutex_unlock(&ctx->mtx);
}

/* 等待受理后的下一张 JPEG，完成 fsync/排他命名后才返回成功。 */
int ipcam_record_save_photo(ipcam_record_ctx_t *ctx, char *path_out, size_t path_sz)
{
    if (!ctx || !ctx->jpeg_rb) return -1;
    char error[128];
    if (storage_check(ctx->storage_root, error, sizeof(error)) != 0) {
        status_error(ctx, error);
        return -1;
    }
    char dir[256];
    int dir_len = snprintf(dir, sizeof(dir), "%s/ipcam-recordings", ctx->storage_root);
    if (dir_len < 0 || (size_t)dir_len >= sizeof(dir)) {
        status_error(ctx, "照片目录路径过长");
        return -1;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        status_error(ctx, "创建照片目录失败");
        return -1;
    }
    pthread_mutex_lock(&ctx->latest_mtx);
    unsigned long previous_seq = ctx->latest_seq;
    /* 拍照命令的受理点在这里；等待其后的下一张有效 JPEG，最多 2 秒。 */
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        deadline.tv_sec = time(NULL) + 2;
        deadline.tv_nsec = 0;
    } else {
        deadline.tv_sec += 2;
    }
    while (ctx->latest_seq == previous_seq && !record_is_shutdown(ctx)) {
        int wait_rc = pthread_cond_timedwait(&ctx->latest_cond, &ctx->latest_mtx, &deadline);
        if (wait_rc != 0) break;
    }
    size_t jpeg_size = (ctx->latest_seq != previous_seq) ? ctx->latest_size : 0;
    unsigned char *jpeg = jpeg_size ? malloc(jpeg_size) : NULL;
    if (jpeg) memcpy(jpeg, ctx->latest_jpeg, jpeg_size);
    pthread_mutex_unlock(&ctx->latest_mtx);
    if (!jpeg || jpeg_size == 0) {
        free(jpeg);
        status_error(ctx, "当前没有有效视频帧");
        return -1;
    }
    /* 拍照也要为 JPEG 本体预留空间，避免只检查固定余量后写到一半满盘。 */
    if (!storage_has_room(ctx->storage_root, jpeg_size)) {
        free(jpeg);
        status_error(ctx, "照片空间不足");
        return -1;
    }
    static unsigned long photo_no;
    unsigned long no = __sync_add_and_fetch(&photo_no, 1);
    char temp[256], final[256];
    int temp_len = snprintf(temp, sizeof(temp), "%s/%lu-photo-%lu.jpg.part",
                            dir, ctx->session_id, no);
    int final_len = snprintf(final, sizeof(final), "%s/%lu-photo-%lu.jpg",
                             dir, ctx->session_id, no);
    if (temp_len < 0 || (size_t)temp_len >= sizeof(temp) ||
        final_len < 0 || (size_t)final_len >= sizeof(final)) {
        free(jpeg);
        status_error(ctx, "照片文件路径过长");
        return -1;
    }
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { free(jpeg); status_error(ctx, "创建照片文件失败"); return -1; }
    int write_ok = write_all_fd(fd, jpeg, jpeg_size) == 0 && fsync(fd) == 0;
    int close_ok = close(fd) == 0;
    free(jpeg);
    /* close 失败同样视为未完成，不能把可能仍在写入的临时文件改正式名。 */
    if (!write_ok || !close_ok || access(final, F_OK) == 0 || rename(temp, final) != 0) {
        unlink(temp); status_error(ctx, "照片保存失败"); return -1;
    }
    if (path_out && path_sz > 0) snprintf(path_out, path_sz, "%s", final);
    return 0;
}

/* 停止消费者并收尾当前 .part，随后释放快照缓存和同步原语。 */
void ipcam_record_stop(ipcam_record_ctx_t *ctx)
{
    if (!ctx) return;
    /* 原子置位后广播条件变量，正在等待下一帧的拍照命令会及时返回。 */
    __sync_lock_test_and_set(&ctx->shutdown_requested, 1);
    pthread_mutex_lock(&ctx->latest_mtx);
    pthread_cond_broadcast(&ctx->latest_cond);
    pthread_mutex_unlock(&ctx->latest_mtx);
    if (ctx->jpeg_rb) ipcam_ring_close(ctx->jpeg_rb);
    if (ctx->thread) { pthread_join(ctx->thread, NULL); ctx->thread = 0; }
    pthread_mutex_lock(&ctx->mtx);
    if (ctx->status.state == IPCAM_RECORD_STARTING ||
        ctx->status.state == IPCAM_RECORD_RECORDING ||
        ctx->status.state == IPCAM_RECORD_STOPPING) {
        ctx->status.state = IPCAM_RECORD_IDLE;
        ctx->status.current_file[0] = '\0';
    }
    pthread_mutex_unlock(&ctx->mtx);
    free(ctx->latest_jpeg);
    ctx->latest_jpeg = NULL;
    ctx->latest_size = ctx->latest_cap = 0;
    pthread_cond_destroy(&ctx->latest_cond);
    pthread_mutex_destroy(&ctx->latest_mtx);
    pthread_mutex_destroy(&ctx->mtx);
}
