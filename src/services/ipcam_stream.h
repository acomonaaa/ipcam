#ifndef IPCAM_STREAM_HTTP_H
#define IPCAM_STREAM_HTTP_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include "ipcam_ringbuffer.h"

/*
 * HTTP MJPEG 推流服务器：
 *   GET /                  → HTML（带 <img src="/stream.mjpg">）
 *   GET /stream.mjpg       → multipart/x-mixed-replace; 每帧前写 boundary + Content-Type + Content-Length + JPEG
 *   GET /snapshot.jpg      → 单帧 JPEG
 *   GET /api/status        → JSON 状态
 *
 * 默认绑定由 param `http_bind_local` 决定（默认 0 → 0.0.0.0）。
 * 可通过环境变量 IPCAM_HTTP_BIND 覆盖。
 *
 * 同时并发客户端上限 = IPCAM_HTTP_MAX_CLIENTS（默认 8）。
 *
 * shutdown 顺序（main.c cleanup_all 负责 ring_close）：
 *   1) running=0，shutdown+close listen_fd
 *   2) join accept_loop
 *   3) wait until detached client threads drain (client_cnt)
 *   4) destroy client_mtx
 */
#define IPCAM_MAX_TRACKED_CLIENTS 32

typedef struct ipcam_stream_ctx_s {
    int                  listen_fd;
    int                  port;
    ipcam_ring_buffer_t *jpeg_rb;     /* 输入：JPEG 字节流 */
    volatile sig_atomic_t *running;
    pthread_t            thread;      /* accept 循环线程（可 join） */

    pthread_mutex_t      client_mtx;    /* 保护 client_cnt + client_threads[] */
    pthread_mutex_t      ring_mtx;      /* 序列化 jpeg_rb 的多 reader 访问 */
    int                  client_cnt;
    pthread_t            client_threads[IPCAM_MAX_TRACKED_CLIENTS];
} ipcam_stream_ctx_t;

int  ipcam_stream_start(ipcam_stream_ctx_t *ctx, ipcam_ring_buffer_t *jpeg_rb,
                        volatile sig_atomic_t *running);
void ipcam_stream_stop(ipcam_stream_ctx_t *ctx);

#endif /* IPCAM_STREAM_HTTP_H */