#define _GNU_SOURCE
#include "ipcam_encode.h"
#include "ipcam_log.h"

#define IPCAM_ENCODE_LOG_MODULE "ENC "

/* ipcam-display-only target 用：
 *   - 标记 ctx->quality = 0 表示 stub（sentinel）
 *   - main.c 检测到 sentinel 后跳过 stream 启动（无 MJPEG）
 *   - daemon 继续以"纯采集 + LCD"模式运行
 */
int ipcam_encode_start(ipcam_encode_ctx_t *ctx,
                       ipcam_ring_buffer_t *in,
                       ipcam_ring_buffer_t *out,
                       int src_w, int src_h,
                       int out_w, int out_h,
                       volatile sig_atomic_t *running)
{
    (void)in; (void)out; (void)src_w; (void)src_h;
    (void)out_w; (void)out_h; (void)running;
    if (ctx) {
        ctx->quality = 0;  /* sentinel：主流程据此跳过 stream */
        /*
         * stub 没有编码线程，必须明确打印运行模式；否则只看到 display
         * 日志时容易误以为 MJPEG 编码线程启动失败。
         */
        MLOGI_M(IPCAM_ENCODE_LOG_MODULE,
                "encode stub active: display-only mode, HTTP stream disabled\n");
    }
    return 0;
}

void ipcam_encode_stop(ipcam_encode_ctx_t *ctx)
{
    (void)ctx;
}
