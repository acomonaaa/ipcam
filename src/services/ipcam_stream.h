#ifndef IPCAM_STREAM_HTTP_H
#define IPCAM_STREAM_HTTP_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include "ipcam_ringbuffer.h"
#include "ipcam_control.h"
#include "ipcam_record.h"
#include "ipcam_display.h"
#include "ipcam_screen.h"

/*
 * HTTP MJPEG 推流服务器：
 *   GET /                  → HTML（带 <img src="/stream.mjpg">）
 *   GET /stream.mjpg       → multipart/x-mixed-replace; 每帧前写 boundary + Content-Type + Content-Length + JPEG
 *   GET /snapshot.jpg      → 单帧 JPEG
 *   GET /api/status        → JSON 状态
 *   GET /api/capabilities  → 已验证能力清单
 *   GET /api/control/result?id=N → 控制请求结果
 *   POST /api/control      → 预览视口、翻转、录像和拍照命令
 *   POST /api/record       → 录像 start/stop
 *   POST /api/photo        → 独立拍照
 *
 * 默认绑定由 param `http_bind_local` 决定（默认 0 → 0.0.0.0）。
 * 可通过环境变量 IPCAM_HTTP_BIND 覆盖。
 *
 * 同时并发客户端上限 = IPCAM_HTTP_MAX_CLIENTS（默认 8）。
 *
 * shutdown 顺序（共享 JPEG ring 由 main.c cleanup_all 负责关闭）：
 *   1) service_running=0，shutdown+close listen_fd
 *   2) join accept_loop
 *   3) shutdown client fd，由客户端线程最终 close
 *   4) 等待 detached client 线程 drain
 *   5) 确认 client_cnt=0 后再销毁同步对象
 */
#define IPCAM_MAX_TRACKED_CLIENTS 32

typedef struct ipcam_stream_ctx_s {
    int                  listen_fd;
    int                  port;
    ipcam_ring_buffer_t *jpeg_rb;     /* 输入：JPEG 字节流 */
    ipcam_control_ctx_t *control;     /* GUI/HTTP 共用的串行控制契约 */
    ipcam_record_ctx_t *recorder;     /* 可选：录像控制与状态 */
    ipcam_display_ctx_t *display;     /* 可选：本地预览视口控制 */
    ipcam_screen_ctx_t *screen;       /* 可选：熄屏策略 */
    volatile sig_atomic_t *running;
    volatile sig_atomic_t service_running; /* 仅 HTTP 服务自身的生命周期 */
    pthread_t            thread;      /* accept 循环线程（可 join） */

    pthread_mutex_t      client_mtx;    /* 保护 client_cnt + client_fds[] */
    pthread_cond_t       client_cond;   /* 客户端线程退出时唤醒 stop 等待者 */
    pthread_mutex_t      ring_mtx;      /* 序列化 jpeg_rb 的多 reader 访问 */
    int                  client_cnt;
    int                  client_fds[IPCAM_MAX_TRACKED_CLIENTS];
} ipcam_stream_ctx_t;

int  ipcam_stream_start(ipcam_stream_ctx_t *ctx, ipcam_ring_buffer_t *jpeg_rb,
                        volatile sig_atomic_t *running);
int  ipcam_stream_start_ex(ipcam_stream_ctx_t *ctx, ipcam_ring_buffer_t *jpeg_rb,
                           volatile sig_atomic_t *running,
                           ipcam_control_ctx_t *control);
void ipcam_stream_set_recorder(ipcam_stream_ctx_t *ctx, ipcam_record_ctx_t *recorder);
void ipcam_stream_set_control(ipcam_stream_ctx_t *ctx, ipcam_control_ctx_t *control);
void ipcam_stream_set_display(ipcam_stream_ctx_t *ctx, ipcam_display_ctx_t *display);
void ipcam_stream_set_screen(ipcam_stream_ctx_t *ctx, ipcam_screen_ctx_t *screen);
void ipcam_stream_stop(ipcam_stream_ctx_t *ctx);

#endif /* IPCAM_STREAM_HTTP_H */
