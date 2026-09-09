#define _GNU_SOURCE
#include "ipcam_encode.h"

#include <string.h>

/* ipcam-display-only target 用：
 *   - 标记 ctx->quality = 0 表示 stub（sentinel）
 *   - main.c 检测到 sentinel 后跳过 stream 启动（无 MJPEG）
 *   - daemon 继续以"纯采集 + LCD"模式运行
 */
int ipcam_encode_start(ipcam_encode_ctx_t *ctx,
                       ipcam_ring_buffer_t *in,
                       ipcam_ring_buffer_t *out,
                       int src_w, int src_h,
                       volatile sig_atomic_t *running)
{
    (void)in; (void)out; (void)src_w; (void)src_h; (void)running;
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
        ctx->quality = 0;  /* sentinel：主流程据此跳过 stream */
        ctx->service_running = 1;
        pthread_mutex_init(&ctx->stats_mtx, NULL);
    }
    return 0;
}

/*
 * display-only 构建也要实现与真实编码器相同的扩展入口，
 * 否则 main.c 为录像广播准备 aux ring 后会在链接阶段缺少符号。
 * stub 仍然只保留 LCD 采集，不消费或关闭任何业务环槽。
 */
int ipcam_encode_start_ex(ipcam_encode_ctx_t *ctx,
                          ipcam_ring_buffer_t *in,
                          ipcam_ring_buffer_t *out,
                          ipcam_ring_buffer_t *aux,
                          int src_w, int src_h,
                          volatile sig_atomic_t *running)
{
    (void)aux;
    return ipcam_encode_start(ctx, in, out, src_w, src_h, running);
}

void ipcam_encode_stop(ipcam_encode_ctx_t *ctx)
{
    if (ctx) {
        ctx->service_running = 0;
        pthread_mutex_destroy(&ctx->stats_mtx);
    }
}

/* display-only 没有编码线程，统一返回零统计以保持主循环接口一致。 */
void ipcam_encode_get_stats(ipcam_encode_ctx_t *ctx, uint64_t *encoded,
                            uint64_t *dropped)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    if (encoded) *encoded = ctx->frames_encoded;
    if (dropped) *dropped = ctx->frames_dropped;
    pthread_mutex_unlock(&ctx->stats_mtx);
}
