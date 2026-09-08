#define _GNU_SOURCE
#include "ipcam_stream.h"
#include "ipcam_log.h"
#include "ipcam_ota.h"
#include "ipcam_param.h"
#include "ipcam_sys.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "ipcam_config.h"

#define BACKLOG          8
#define READ_TIMEOUT     5
#define MAX_CLIENTS_DEF  8

#define MAX_PATH 64
#define MAX_REQ  2048
#define IPCAM_STREAM_LOG_MODULE "HTTP"

typedef struct ipcam_stream_client_arg_s {
    int                   cfd;
    int                   slot;
    ipcam_ring_buffer_t  *jpeg_rb;
    ipcam_stream_ctx_t   *ctx;
} ipcam_stream_client_arg_t;

static void stream_destroy_sync(ipcam_stream_ctx_t *ctx)
{
    /*
     * 调用者必须先确认 accept/client 线程都已退出；同步对象一旦销毁，
     * 任何仍持有 ctx 的线程都会进入未定义行为，因此这里不负责等待。
     */
    pthread_cond_destroy(&ctx->client_cond);
    pthread_mutex_destroy(&ctx->ring_mtx);
    pthread_mutex_destroy(&ctx->client_mtx);
}

static ssize_t safe_write(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, (const char *)buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += n;
    }
    return (ssize_t)sent;
}

static const char *serve_index_body =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<title>ipcam</title>"
    "<style>body{margin:0;background:#000;color:#fff;font-family:sans-serif;text-align:center}"
    "h3{margin:6px}img{max-width:100%;display:block;margin:0 auto}</style></head>"
    "<body><h3>ipcam live</h3>"
    "<img src='/stream.mjpg' alt='live stream'>"
    "</body></html>";

static void serve_index(int fd)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     strlen(serve_index_body));
    if (n < 0 || n >= (int)sizeof(hdr)) return;
    safe_write(fd, hdr, (size_t)n);
    safe_write(fd, serve_index_body, strlen(serve_index_body));
}

static void serve_status(int fd, ipcam_ring_buffer_t *jpeg_rb)
{
    int cnt = jpeg_rb ? ipcam_ring_count(jpeg_rb) : 0;
    char json[512];
    int n = ipcam_param_to_json(json, sizeof(json) - 64);
    if (n < 0) n = 0;
    /* 在 param JSON 后追加 ring_count 字段 */
    char buf[640];
    int m = snprintf(buf, sizeof(buf),
                     "{\"ring_count\":%d,%.*s",
                     cnt, n > 0 ? (int)(strchr(json, '{') - json + 1) : 0, json);
    (void)m;
    /* 上面的拼接不够稳；直接重新 snprintf 一次完整 */
    m = snprintf(buf, sizeof(buf),
                 "{\"ring_count\":%d,"
                 "\"model\":\"%s\",\"swver\":\"%s\","
                 "\"net_mode\":%u,\"capture_w\":%u,\"capture_h\":%u,"
                 "\"jpeg_q\":%u,\"http_port\":%u}\n",
                 cnt,
                 ipcam_param_get_model(), ipcam_param_get_swver(),
                 ipcam_param_get_net_mode(),
                 ipcam_param_get_capture_w(), ipcam_param_get_capture_h(),
                 ipcam_param_get_jpeg_quality(), ipcam_param_get_http_port());
    if (m < 0 || m >= (int)sizeof(buf)) {
        const char *err = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, err, strlen(err));
        return;
    }
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %d\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      m);
    if (hn < 0 || hn >= (int)sizeof(hdr)) return;
    safe_write(fd, hdr, (size_t)hn);
    safe_write(fd, buf, (size_t)m);
    (void)json;
}

/*
 * /api/config GET — 返回所有 param 字段 JSON。
 */
static void serve_config_get(int fd)
{
    char body[768];
    int n = ipcam_param_to_json(body, sizeof(body));
    if (n < 0) {
        const char *e = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, e, strlen(e));
        return;
    }
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n"
                      "\r\n", n);
    safe_write(fd, hdr, (size_t)hn);
    safe_write(fd, body, (size_t)n);
}

/*
 * /api/config POST — 解析 key=value&key=value 格式，逐项 set。
 * 也接受 JSON body（首选；简化为 key":"value 对）。
 * 简化：仅接受 application/x-www-form-urlencoded 与裸 key=value 串。
 */
static void serve_config_post(int fd, const char *body, size_t body_len)
{
    int changes = 0, errors = 0;
    char *work = strndup(body, body_len);
    if (!work) {
        const char *e = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, e, strlen(e));
        return;
    }

    char *saveptr = NULL;
    for (char *p = strtok_r(work, "&", &saveptr); p; p = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        /* urldecode（最小集：+ → space，%XX → byte） */
        char *v = val;
        char *w = val;
        while (*v) {
            if (*v == '+') { *w++ = ' '; v++; }
            else if (*v == '%' && v[1] && v[2]) {
                unsigned hi = (unsigned)(v[1] >= '0' && v[1] <= '9' ? v[1]-'0' :
                                       v[1] >= 'a' && v[1] <= 'f' ? v[1]-'a'+10 :
                                       v[1] >= 'A' && v[1] <= 'F' ? v[1]-'A'+10 : 0);
                unsigned lo = (unsigned)(v[2] >= '0' && v[2] <= '9' ? v[2]-'0' :
                                       v[2] >= 'a' && v[2] <= 'f' ? v[2]-'a'+10 :
                                       v[2] >= 'A' && v[2] <= 'F' ? v[2]-'A'+10 : 0);
                *w++ = (char)((hi << 4) | lo);
                v += 3;
            } else {
                *w++ = *v++;
            }
        }
        *w = '\0';

        int rc = -1;
        if      (!strcmp(key, "wifi_ssid"))    rc = ipcam_param_set_wifi_ssid(val);
        else if (!strcmp(key, "wifi_psk"))     rc = ipcam_param_set_wifi_psk(val);
        else if (!strcmp(key, "apn"))          rc = ipcam_param_set_apn(val);
        else if (!strcmp(key, "net_mode"))     rc = ipcam_param_set_net_mode((uint8_t)atoi(val));
        else if (!strcmp(key, "capture_w"))    rc = ipcam_param_set_capture_w((uint16_t)atoi(val));
        else if (!strcmp(key, "capture_h"))    rc = ipcam_param_set_capture_h((uint16_t)atoi(val));
        else if (!strcmp(key, "out_w"))        rc = ipcam_param_set_out_w((uint16_t)atoi(val));
        else if (!strcmp(key, "out_h"))        rc = ipcam_param_set_out_h((uint16_t)atoi(val));
        else if (!strcmp(key, "jpeg_quality")) rc = ipcam_param_set_jpeg_quality((uint8_t)atoi(val));
        else if (!strcmp(key, "target_fps"))   rc = ipcam_param_set_target_fps((uint8_t)atoi(val));
        else if (!strcmp(key, "http_port"))    rc = ipcam_param_set_http_port((uint16_t)atoi(val));
        else if (!strcmp(key, "log_level"))    rc = ipcam_param_set_log_level((uint8_t)atoi(val));
        else if (!strcmp(key, "http_bind_local")) rc = ipcam_param_set_http_bind_local((uint8_t)atoi(val));
        else {
            MLOGW_M(IPCAM_STREAM_LOG_MODULE,
                    "config_post: unknown key '%s'\n", key);
            errors++;
            continue;
        }

        if (rc == 0) {
            MLOGI_M(IPCAM_STREAM_LOG_MODULE,
                    "config_post: %s = %s (saved)\n", key, val);
            changes++;
        } else {
            MLOGW_M(IPCAM_STREAM_LOG_MODULE,
                    "config_post: %s = %s rejected\n", key, val);
            errors++;
        }
    }
    free(work);

    char body_out[256];
    int n = snprintf(body_out, sizeof(body_out),
                     "{\"changes\":%d,\"errors\":%d}\n", changes, errors);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n"
                      "\r\n", n);
    safe_write(fd, hdr, (size_t)hn);
    safe_write(fd, body_out, (size_t)n);
}

static int read_http_request(int fd, char *buf, size_t buf_sz)
{
    size_t got = 0;
    while (got < buf_sz - 1) {
        ssize_t n = read(fd, buf + got, buf_sz - 1 - got);
        if (n <= 0) return -1;
        got += (size_t)n;
        buf[got] = '\0';
        if (strstr(buf, "\r\n\r\n")) return (int)got;
    }
    return -1;
}

/*
 * 组装 HTTP POST body。
 * 读请求头时可能在 "\r\n\r\n" 之后已经带上部分/全部 body；若再按 Content-Length
 * 从 socket 整段重读，常见 curl 一次发包会卡住超时，导致 /api/config、/api/ota 空 body。
 * 做法：先拷贝缓冲区内已有前缀，再补读剩余字节；不足则失败（由调用方回 400）。
 * 成功返回 0 且 *body_out 需由调用方 free；失败返回 -1（*body_out 为 NULL）。
 */
static int read_http_body(int fd, const char *req, int req_len,
                          int content_len, char **body_out)
{
    *body_out = NULL;
    if (content_len <= 0 || content_len > 4096) return -1;

    char *body = malloc((size_t)content_len + 1);
    if (!body) return -1;

    const char *hdr_end = strstr(req, "\r\n\r\n");
    size_t got = 0;
    if (hdr_end) {
        const char *pre = hdr_end + 4;
        size_t pre_len = (size_t)(req_len - (int)(pre - req));
        if (pre_len > (size_t)content_len) pre_len = (size_t)content_len;
        if (pre_len > 0) {
            memcpy(body, pre, pre_len);
            got = pre_len;
        }
    }

    while ((int)got < content_len) {
        ssize_t n = read(fd, body + got, (size_t)content_len - got);
        if (n <= 0) {
            free(body);
            return -1;
        }
        got += (size_t)n;
    }
    body[content_len] = '\0';
    *body_out = body;
    return 0;
}

static void serve_snapshot(int fd, ipcam_ring_buffer_t *jpeg_rb, pthread_mutex_t *ring_mtx)
{
    ipcam_frame_t f;
    unsigned char *local = NULL;
    size_t local_cap = 0;
    size_t frame_size = 0;

    pthread_mutex_lock(ring_mtx);
    int got = ipcam_ring_get(jpeg_rb, &f);
    if (got != 0) {
        pthread_mutex_unlock(ring_mtx);
        const char *e = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, e, strlen(e));
        return;
    }

    /* 拷贝到本地 buffer，脱离 ring */
    if (f.size > local_cap) {
        unsigned char *nb = realloc(local, f.size);
        if (!nb) {
            ipcam_ring_release(jpeg_rb);
            pthread_mutex_unlock(ring_mtx);
            return;
        }
        local = nb;
        local_cap = f.size;
    }
    if (f.size > 0 && f.rawData) memcpy(local, f.rawData, f.size);
    frame_size = f.size;
    ipcam_ring_release(jpeg_rb);
    pthread_mutex_unlock(ring_mtx);

    /* 现在独立写 socket（ring 已 release，encode 可继续） */
    char header[256];
    int hn = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/jpeg\r\n"
                      "Content-Length: %zu\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      frame_size);
    if (hn >= 0 && hn < (int)sizeof(header) &&
        safe_write(fd, header, (size_t)hn) >= 0 &&
        frame_size > 0 && safe_write(fd, local, frame_size) >= 0) {
        /* ok */
    }
    free(local);
}

static void serve_stream(int fd, ipcam_ring_buffer_t *jpeg_rb, pthread_mutex_t *ring_mtx,
                         volatile sig_atomic_t *running, unsigned long sid)
{
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=ipcam\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (safe_write(fd, hdr, strlen(hdr)) < 0) {
        MLOGW_M(IPCAM_STREAM_LOG_MODULE,
                "stream header write failed sid=%lu\n", sid);
        return;
    }

    /*
     * 关键：ring_mtx 只保护 ring_get+ring_release，不在网络 I/O 期间持有。
     * 否则一个慢客户端会阻塞所有其它 reader（包括 snapshot 和其它 stream）。
     *
     * 流程：
     *   1) 短持锁：get frame + memcpy 到本地缓冲（避免 race）
     *   2) 释放锁：encode 线程可以继续写下一帧
     *   3) 无锁：safe_write 把本地缓冲写到 socket（最多 SO_SNDTIMEO 秒）
     */
    unsigned char *local = NULL;
    size_t local_cap = 0;
    unsigned long frames = 0, report_frames = 0;
    struct timeval t0, last_report, now;
    gettimeofday(&t0, NULL);
    last_report = t0;

    while (*running) {
        ipcam_frame_t f;
        pthread_mutex_lock(ring_mtx);
        int gr = ipcam_ring_get(jpeg_rb, &f);
        if (gr != 0) {
            pthread_mutex_unlock(ring_mtx);
            usleep(50 * 1000);
            continue;
        }

        /* 拷贝到本地 buffer（脱离 ring） */
        if (f.size > local_cap) {
            unsigned char *nb = realloc(local, f.size);
            if (!nb) {
                MLOGW_M(IPCAM_STREAM_LOG_MODULE,
                        "stream buffer realloc failed sid=%lu bytes=%zu\n",
                        sid, f.size);
                ipcam_ring_release(jpeg_rb);
                pthread_mutex_unlock(ring_mtx);
                usleep(50 * 1000);
                continue;
            }
            local = nb;
            local_cap = f.size;
        }
        if (f.size > 0 && f.rawData) memcpy(local, f.rawData, f.size);
        size_t frame_size = f.size;
        ipcam_ring_release(jpeg_rb);
        pthread_mutex_unlock(ring_mtx);

        /* 现在独立写 socket（ring 已 release，encode 可继续） */
        char part_hdr[256];
        int hn = snprintf(part_hdr, sizeof(part_hdr),
                          "--ipcam\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          frame_size);
        if (hn < 0 || hn >= (int)sizeof(part_hdr)) goto cleanup;
        if (safe_write(fd, part_hdr, (size_t)hn) < 0) goto cleanup;
        if (frame_size > 0 && safe_write(fd, local, frame_size) < 0) goto cleanup;
        if (safe_write(fd, "\r\n", 2) < 0) goto cleanup;

        frames++;
        /* 首个 JPEG 到达客户端时记录实际序号和大小，便于区分“服务已监听”和“已出图”。 */
        if (frames == 1) {
            MLOGI_M(IPCAM_STREAM_LOG_MODULE,
                    "stream first frame sid=%lu seq=%lu bytes=%zu\n",
                    sid, f.seqNo, frame_size);
        }

        /* 慢客户端或网络异常时，周期统计能显示该会话是否持续收到 JPEG。 */
        gettimeofday(&now, NULL);
        double report_sec = (now.tv_sec - last_report.tv_sec) +
                            (now.tv_usec - last_report.tv_usec) / 1e6;
        if (report_sec >= 5.0) {
            unsigned long interval_frames = frames - report_frames;
            MLOGI_M(IPCAM_STREAM_LOG_MODULE,
                    "stream stats sid=%lu interval=%.1fs fps=%.1f frames=%lu rb=%d\n",
                    sid, report_sec,
                    report_sec > 0 ? interval_frames / report_sec : 0,
                    frames, ipcam_ring_count(jpeg_rb));
            last_report = now;
            report_frames = frames;
        }
        continue;
cleanup:
        gettimeofday(&now, NULL);
        double sec = (now.tv_sec - t0.tv_sec) +
                     (now.tv_usec - t0.tv_usec) / 1e6;
        MLOGI_M(IPCAM_STREAM_LOG_MODULE,
                "stream worker exit sid=%lu frames=%lu avg_fps=%.1f\n",
                sid, frames, sec > 0 ? frames / sec : 0);
        free(local);
        return;
    }

    gettimeofday(&now, NULL);
    double sec = (now.tv_sec - t0.tv_sec) +
                 (now.tv_usec - t0.tv_usec) / 1e6;
    MLOGI_M(IPCAM_STREAM_LOG_MODULE,
            "stream worker exit sid=%lu frames=%lu avg_fps=%.1f\n",
            sid, frames, sec > 0 ? frames / sec : 0);
    free(local);
}

/*
 * 解析 HTTP 首行，返回 method（"GET"/"POST"/...）和 path。
 * 返回 0 成功；-2 method 非法；-3 path 过长；-4 method 字段过长。
 */
static int parse_http_request_line(const char *req, char *method, size_t method_sz,
                                   char *path_buf, size_t path_sz)
{
    /* 防御性初始化：保证所有失败返回都使 method 是合法的 NUL-terminated 空串 */
    if (method && method_sz > 0) method[0] = '\0';
    if (path_buf && path_sz > 0) path_buf[0] = '\0';

    /* 提取 method：第一个 token */
    const char *p = req;
    const char *end = p;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n') end++;
    size_t mlen = (size_t)(end - p);
    if (mlen == 0 || mlen >= method_sz) return -4;
    memcpy(method, p, mlen);
    method[mlen] = '\0';

    if (*end != ' ') return -2;
    p = end + 1;
    while (*p == ' ') p++;
    end = p;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n') end++;
    size_t plen = (size_t)(end - p);
    if (plen == 0 || plen >= path_sz) return -3;
    memcpy(path_buf, p, plen);
    path_buf[plen] = '\0';
    return 0;
}

/*
 * 解析 Content-Length header（\r\n 风格）
 */
static int get_content_length(const char *req)
{
    const char *p = req;
    while ((p = strcasestr(p, "Content-Length:"))) {
        if (p != req && *(p - 1) != '\n') { p += 14; continue; }
        p += 14;
        while (*p == ' ') p++;
        return atoi(p);
    }
    return 0;
}

/* mo_live 风格：分配 session id（单调递增） */
static unsigned long next_session_id(void)
{
    static _Atomic unsigned long g_sid = 0;
    return atomic_fetch_add(&g_sid, 1) + 1;
}

/* === /api/version (GET) === */
static void serve_version(int fd)
{
    struct sysinfo si;
    long uptime = (sysinfo(&si) == 0) ? (long)si.uptime : -1;
    char json[256];
    int n = snprintf(json, sizeof(json),
        "{\"model\":\"%s\",\"swver\":\"%s\","
        "\"git\":\"%s\",\"build\":\"%s %s\","
        "\"uptime_s\":%ld}\n",
        ipcam_param_get_model(), ipcam_param_get_swver(),
        LIBIPCAM_GIT_INFO, __DATE__, __TIME__, uptime);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", n);
    safe_write(fd, hdr, (size_t)hn);
    safe_write(fd, json, (size_t)n);
}

/* === /healthz (GET) → 用于 init.d 回滚看门狗 === */
static void serve_healthz(int fd, ipcam_stream_ctx_t *ctx)
{
    int running = ctx && ctx->running && *ctx->running;
    int closed = !ctx || !ctx->jpeg_rb || ipcam_ring_is_closed(ctx->jpeg_rb);
    int queue_count = (ctx && ctx->jpeg_rb) ? ipcam_ring_count(ctx->jpeg_rb) : 0;
    int ok = running && !closed;
    char body[160];
    int body_len = snprintf(body, sizeof(body),
                            "{\"ok\":%s,\"running\":%s,\"jpeg_queue\":%d}\n",
                            ok ? "true" : "false",
                            running ? "true" : "false",
                            queue_count);
    if (body_len < 0 || body_len >= (int)sizeof(body)) return;
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        ok ? "200 OK" : "503 Service Unavailable", (size_t)body_len);
    if (hn < 0 || hn >= (int)sizeof(hdr)) return;
    safe_write(fd, hdr, (size_t)hn);
    safe_write(fd, body, (size_t)body_len);
}

/* === /api/ota (GET) === */
static void serve_ota_status(int fd)
{
    ipcam_ota_result_t r;
    ipcam_ota_get_status(&r);
    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"state\":%d,\"message\":\"%s\","
        "\"current_version\":\"%s\",\"pending_version\":\"%s\","
        "\"progress\":%u}\n",
        (int)r.state, r.message,
        r.current_version, r.pending_version, r.progress);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", n);
    safe_write(fd, hdr, (size_t)hn);
    safe_write(fd, json, (size_t)n);
}

/* === /api/ota (POST) body parser: url=...&sha256=... === */
static void serve_ota_post(int fd, const char *body, size_t body_len)
{
    /* 必须 root 才能触发 OTA —— 任意 LAN 客户端不能借此重刷设备 */
    if (geteuid() != 0) {
        const char *m = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
        return;
    }

    char *work = strndup(body, body_len);
    if (!work) {
        const char *m = "HTTP/1.1 500\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
        return;
    }
    char url[512] = "", sha[80] = "";
    char *saveptr = NULL;
    for (char *p = strtok_r(work, "&", &saveptr); p; p = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p, *val = eq + 1;
        /* urldecode 最小集 */
        char *v = val, *w = val;
        while (*v) {
            if (*v == '+') { *w++ = ' '; v++; }
            else if (*v == '%' && v[1] && v[2]) {
                unsigned hi = (v[1] >= '0' && v[1] <= '9') ? v[1]-'0' :
                               (v[1] >= 'a' && v[1] <= 'f') ? v[1]-'a'+10 :
                               (v[1] >= 'A' && v[1] <= 'F') ? v[1]-'A'+10 : 0;
                unsigned lo = (v[2] >= '0' && v[2] <= '9') ? v[2]-'0' :
                               (v[2] >= 'a' && v[2] <= 'f') ? v[2]-'a'+10 :
                               (v[2] >= 'A' && v[2] <= 'F') ? v[2]-'A'+10 : 0;
                *w++ = (char)((hi << 4) | lo);
                v += 3;
            } else { *w++ = *v++; }
        }
        *w = '\0';
        if (!strcmp(key, "url"))    snprintf(url, sizeof(url), "%s", val);
        else if (!strcmp(key, "sha256")) snprintf(sha, sizeof(sha), "%s", val);
    }
    free(work);

    if (url[0] == '\0') {
        const char *m = "HTTP/1.1 400 missing url\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
        return;
    }

    MLOGI_M(IPCAM_STREAM_LOG_MODULE,
            "api POST /api/ota url=%s sha256=%s\n", url, sha);
    ipcam_ota_result_t r;
    memset(&r, 0, sizeof(r));
    int rc = ipcam_ota_from_url(url, sha[0] ? sha : NULL, &r);

    char json[256];
    int n = snprintf(json, sizeof(json),
        "{\"ok\":%s,\"state\":%d,\"message\":\"%s\"}\n",
        rc == 0 ? "true" : "false", (int)r.state, r.message);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",
        rc == 0 ? "200 OK" : "400 Bad Request", n);
    safe_write(fd, hdr, (size_t)hn);
    safe_write(fd, json, (size_t)n);
}

/* === /api/reboot (POST) === */
static void serve_reboot_post(int fd)
{
    if (geteuid() != 0) {
        const char *m = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
        return;
    }
    const char *m = "HTTP/1.1 200 OK\r\nContent-Length: 19\r\nConnection: close\r\n\r\nrebooting in 2s\n";
    safe_write(fd, m, strlen(m));
    /* 异步 reboot，避免关闭连接 */
    pid_t pid = fork();
    if (pid == 0) {
        sleep(2);
        sync();
        reboot(RB_AUTOBOOT);
        _exit(0);
    }
}

static void handle_client(int fd, ipcam_ring_buffer_t *jpeg_rb, ipcam_stream_ctx_t *ctx)
{
    char req[MAX_REQ];
    char method[8] = "";
    char path[MAX_PATH] = "";

    struct timeval rcv_tv = { .tv_sec = READ_TIMEOUT, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    struct timeval snd_tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

    int req_len = read_http_request(fd, req, sizeof(req));
    if (req_len <= 0) {
        return;
    }

    int rc = parse_http_request_line(req, method, sizeof(method), path, sizeof(path));
    if (rc == -2) {
        const char *m = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
    } else if (rc == -3) {
        const char *m = "HTTP/1.1 414 URI Too Long\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
    } else if (rc == -4) {
        const char *m = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
    } else if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        const char *m = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        serve_index(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/stream.mjpg") == 0) {
        unsigned long sid = next_session_id();
        MLOGI_M(IPCAM_STREAM_LOG_MODULE,
                "stream session start sid=%lu path=%s\n", sid, path);
        serve_stream(fd, jpeg_rb, &ctx->ring_mtx, ctx->running, sid);
        MLOGI_M(IPCAM_STREAM_LOG_MODULE, "stream session end   sid=%lu\n", sid);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/snapshot.jpg") == 0) {
        serve_snapshot(fd, jpeg_rb, &ctx->ring_mtx);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        serve_status(fd, jpeg_rb);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/config") == 0) {
        MLOGI_M(IPCAM_STREAM_LOG_MODULE, "api GET /api/config\n");
        serve_config_get(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/version") == 0) {
        serve_version(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/healthz") == 0) {
        serve_healthz(fd, ctx);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ota") == 0) {
        serve_ota_status(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ota") == 0) {
        int content_len = get_content_length(req);
        char *body = NULL;
        if (read_http_body(fd, req, req_len, content_len, &body) != 0) {
            const char *m = (content_len <= 0 || content_len > 4096)
                ? "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n"
                : "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            safe_write(fd, m, strlen(m));
        } else {
            serve_ota_post(fd, body, (size_t)content_len);
            free(body);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reboot") == 0) {
        serve_reboot_post(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/config") == 0) {
        int content_len = get_content_length(req);
        char *body = NULL;
        if (read_http_body(fd, req, req_len, content_len, &body) != 0) {
            const char *m = (content_len <= 0 || content_len > 4096)
                ? "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n"
                : "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            safe_write(fd, m, strlen(m));
        } else {
            MLOGI_M(IPCAM_STREAM_LOG_MODULE,
                    "api POST /api/config (%d bytes)\n", content_len);
            serve_config_post(fd, body, (size_t)content_len);
            free(body);
        }
    } else {
        const char *m = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m));
    }

}

static void *client_thread(void *arg)
{
    ipcam_stream_client_arg_t *a = arg;
    ipcam_stream_ctx_t *ctx = a->ctx;
    handle_client(a->cfd, a->jpeg_rb, a->ctx);

    /*
     * socket 的最终 close 由拥有该 fd 的客户端线程执行；stop 只做
     * shutdown 来唤醒网络 I/O，避免两个线程同时 close 后 fd 号码被复用。
     */
    close(a->cfd);

    pthread_mutex_lock(&ctx->client_mtx);
    if (a->slot >= 0 && a->slot < IPCAM_MAX_TRACKED_CLIENTS) {
        ctx->client_fds[a->slot] = -1;
    }
    if (ctx->client_cnt > 0) ctx->client_cnt--;
    pthread_cond_broadcast(&ctx->client_cond);
    pthread_mutex_unlock(&ctx->client_mtx);

    free(a);
    return NULL;
}

static int acquire_client_slot(ipcam_stream_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->client_mtx);
    int max_clients = IPCAM_MAX_TRACKED_CLIENTS;  /* 受 client_fds[] 容量限制 */
    const char *e = getenv("IPCAM_HTTP_MAX_CLIENTS");
    if (e && *e) {
        int v = atoi(e);
        if (v > 0 && v <= IPCAM_MAX_TRACKED_CLIENTS) max_clients = v;
    }
    int slot = -1;
    if (ctx->client_cnt < max_clients) {
        for (int i = 0; i < IPCAM_MAX_TRACKED_CLIENTS; i++) {
            if (ctx->client_fds[i] == -1) {
                /* -2 表示已预留但尚未把真实 fd 写入槽位。 */
                ctx->client_fds[i] = -2;
                ctx->client_cnt++;
                slot = i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&ctx->client_mtx);
    return slot;
}

static void release_client_slot(ipcam_stream_ctx_t *ctx, int slot)
{
    if (slot < 0 || slot >= IPCAM_MAX_TRACKED_CLIENTS) return;

    pthread_mutex_lock(&ctx->client_mtx);
    if (ctx->client_fds[slot] >= 0) close(ctx->client_fds[slot]);
    ctx->client_fds[slot] = -1;
    if (ctx->client_cnt > 0) ctx->client_cnt--;
    pthread_cond_broadcast(&ctx->client_cond);
    pthread_mutex_unlock(&ctx->client_mtx);
}

static void *accept_loop(void *arg)
{
    ipcam_stream_ctx_t *ctx = arg;

    while (*ctx->running) {
        struct sockaddr_in cli_addr;
        socklen_t addrlen = sizeof(cli_addr);
        int cfd = accept(ctx->listen_fd, (struct sockaddr *)&cli_addr, &addrlen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!*ctx->running) break;
            MLOGE_M(IPCAM_STREAM_LOG_MODULE, "accept: %s\n", strerror(errno));
            usleep(100 * 1000);
            continue;
        }

        int slot = acquire_client_slot(ctx);
        if (slot < 0) {
            char ipbuf[32];
            inet_ntop(AF_INET, &cli_addr.sin_addr, ipbuf, sizeof(ipbuf));
            MLOGW_M(IPCAM_STREAM_LOG_MODULE,
                  "reject client %s:%d (max reached)\n",
                  ipbuf, ntohs(cli_addr.sin_port));
            const char *m = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
            safe_write(cfd, m, strlen(m));
            close(cfd);
            continue;
        }

        pthread_mutex_lock(&ctx->client_mtx);
        ctx->client_fds[slot] = cfd;
        int active_clients = ctx->client_cnt;
        pthread_mutex_unlock(&ctx->client_mtx);

        char ipbuf[32];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ipbuf, sizeof(ipbuf));
        MLOGI_M(IPCAM_STREAM_LOG_MODULE,
              "client %s:%d connected (active=%d)\n",
              ipbuf, ntohs(cli_addr.sin_port), active_clients);

        ipcam_stream_client_arg_t *a = malloc(sizeof(*a));
        if (!a) {
            release_client_slot(ctx, slot);
            continue;
        }
        a->cfd = cfd;
        a->slot = slot;
        a->jpeg_rb = ctx->jpeg_rb;
        a->ctx = ctx;

        /* cfd FD_CLOEXEC：未来若 fork exec 不会泄漏 */
        int flags = fcntl(cfd, F_GETFD);
        if (flags >= 0) fcntl(cfd, F_SETFD, flags | FD_CLOEXEC);

        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, a) != 0) {
            MLOGE_M(IPCAM_STREAM_LOG_MODULE, "pthread_create client\n");
            free(a);
            release_client_slot(ctx, slot);
            continue;
        }
        /*
         * 客户端线程继续使用 detached 模式，但 stop 会先 shutdown 所有
         * 客户端 fd，再用条件变量等到 client_cnt=0，保证 ctx 和锁销毁时
         * 不再有客户端线程访问它们。
         */
        pthread_detach(t);
    }
    return NULL;
}

int ipcam_stream_start(ipcam_stream_ctx_t *ctx, ipcam_ring_buffer_t *jpeg_rb,
                       volatile sig_atomic_t *running)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->jpeg_rb = jpeg_rb;
    ctx->running = running;
    ctx->port = ipcam_param_get_http_port();  /* BCF2 风格：port 从 param 取 */
    ctx->listen_fd = -1;
    if (pthread_mutex_init(&ctx->client_mtx, NULL) != 0) return -1;
    if (pthread_mutex_init(&ctx->ring_mtx, NULL) != 0) {
        pthread_mutex_destroy(&ctx->client_mtx);
        return -1;
    }
    if (pthread_cond_init(&ctx->client_cond, NULL) != 0) {
        pthread_mutex_destroy(&ctx->ring_mtx);
        pthread_mutex_destroy(&ctx->client_mtx);
        return -1;
    }
    for (int i = 0; i < IPCAM_MAX_TRACKED_CLIENTS; i++) ctx->client_fds[i] = -1;

    /* 默认绑 127.0.0.1；IPCAM_HTTP_BIND 可覆盖（"0.0.0.0" 暴露给全网） */
    const char *bind_ip = getenv("IPCAM_HTTP_BIND");
    if (!bind_ip || !*bind_ip) {
        bind_ip = ipcam_param_get_http_bind_local() ? "127.0.0.1" : "0.0.0.0";
    }

    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        MLOGE_M(IPCAM_STREAM_LOG_MODULE, "socket: %s\n", strerror(errno));
        stream_destroy_sync(ctx);
        return -1;
    }
    int yes = 1;
    setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int flags = fcntl(ctx->listen_fd, F_GETFD);
    if (flags >= 0) fcntl(ctx->listen_fd, F_SETFD, flags | FD_CLOEXEC);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        MLOGE_M(IPCAM_STREAM_LOG_MODULE, "invalid bind ip: %s\n", bind_ip);
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        stream_destroy_sync(ctx);
        return -1;
    }
    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        MLOGE_M(IPCAM_STREAM_LOG_MODULE,
              "bind %s:%d: %s\n", bind_ip, ctx->port, strerror(errno));
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        stream_destroy_sync(ctx);
        return -1;
    }
    if (listen(ctx->listen_fd, BACKLOG) < 0) {
        MLOGE_M(IPCAM_STREAM_LOG_MODULE, "listen: %s\n", strerror(errno));
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        stream_destroy_sync(ctx);
        return -1;
    }
    MLOGI_M(IPCAM_STREAM_LOG_MODULE,
          "HTTP MJPEG server listening on %s:%d (max_clients=%d)\n",
          bind_ip, ctx->port, IPCAM_MAX_TRACKED_CLIENTS);

    if (pthread_create(&ctx->thread, NULL, accept_loop, ctx) != 0) {
        MLOGE_M(IPCAM_STREAM_LOG_MODULE, "pthread_create accept\n");
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        stream_destroy_sync(ctx);
        return -1;
    }
    return 0;
}

void ipcam_stream_stop(ipcam_stream_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->running) *ctx->running = 0;

    /* 关 listen_fd 让 accept() 立刻失败退出 */
    if (ctx->listen_fd >= 0) {
        shutdown(ctx->listen_fd, SHUT_RDWR);
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }

    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }

    /*
     * detached 线程没有 join 句柄，因此先 shutdown 它们的 socket，
     * 再无超时等待计数归零。超时后直接销毁锁会让客户端收尾路径访问
     * 已失效的 ctx；条件变量等待保证销毁同步对象前线程已经退出。
     */
    pthread_mutex_lock(&ctx->client_mtx);
    for (int i = 0; i < IPCAM_MAX_TRACKED_CLIENTS; i++) {
        if (ctx->client_fds[i] >= 0) shutdown(ctx->client_fds[i], SHUT_RDWR);
    }
    pthread_mutex_unlock(&ctx->client_mtx);

    /*
     * ring_close 唤醒正在等待下一帧的 client；main.c 可能已经提前关闭，
     * 这里重复调用是幂等的，且发生在 accept loop 停止、客户端 fd 已 shutdown 之后。
     */
    if (ctx->jpeg_rb) ipcam_ring_close(ctx->jpeg_rb);

    pthread_mutex_lock(&ctx->client_mtx);
    while (ctx->client_cnt > 0) {
        pthread_cond_wait(&ctx->client_cond, &ctx->client_mtx);
    }
    pthread_mutex_unlock(&ctx->client_mtx);

    stream_destroy_sync(ctx);
}
