#define _GNU_SOURCE
/* HTTP 请求、MJPEG 会话和客户端生命周期归入 HTTP 模块。 */
#define IPCAM_LOG_MODULE "HTTP"
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

typedef struct ipcam_stream_client_arg_s {
    int                   cfd;
    int                   slot;
    ipcam_ring_buffer_t  *jpeg_rb;
    ipcam_stream_ctx_t   *ctx;
} ipcam_stream_client_arg_t;

/*
 * stream_start_ex 成功初始化的三个同步对象必须成组销毁；统一收口失败
 * 路径，避免新增条件变量后某个 bind/listen 分支遗漏清理。
 */
static void stream_destroy_sync(ipcam_stream_ctx_t *ctx)
{
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
            MLOGW("socket write failed: fd=%d len=%zu errno=%d(%s)\n",
                  fd, len - sent, errno, strerror(errno));
            return -1;
        }
        if (n == 0) {
            MLOGW("socket write returned zero: fd=%d len=%zu\n", fd, len - sent);
            return -1;
        }
        sent += n;
    }
    return (ssize_t)sent;
}

/* 根页面只负责呈现直播：让 4:3 图像占满视口可用空间，超出的宽度用黑边保留比例。 */
static const char *serve_index_body =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
    "<style>html,body{width:100%;height:100%;margin:0;overflow:hidden;background:#000}"
    "img{width:100vw;height:100vh;display:block;object-fit:contain;background:#000}</style></head>"
    "<body>"
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

/* 汇总控制器快照并返回实际帧参数、网络、存储和队列统计。 */
static void serve_status(int fd, ipcam_ring_buffer_t *jpeg_rb,
                         ipcam_record_ctx_t *rec,
                         ipcam_control_ctx_t *control)
{
    int cnt = jpeg_rb ? ipcam_ring_count(jpeg_rb) : 0;
    ipcam_record_status_t rst;
    memset(&rst, 0, sizeof(rst));
    if (rec) ipcam_record_get_status(rec, &rst);
    ipcam_control_status_t cst;
    memset(&cst, 0, sizeof(cst));
    int have_control = control && ipcam_control_get_status(control, &cst) == 0;
    if (have_control) {
        rst = cst.record;
        cnt = cst.jpeg_live_frames;
    }
    uint16_t status_w = have_control ? cst.video.width : ipcam_param_get_capture_w();
    uint16_t status_h = have_control ? cst.video.height : ipcam_param_get_capture_h();
    uint8_t status_target_fps = have_control ? cst.video.target_fps : ipcam_param_get_target_fps();
    uint32_t status_capture_fps = have_control ? cst.capture_fps : 0;
    uint32_t status_output_fps = have_control ? cst.output_fps : 0;
    uint8_t status_q = have_control ? cst.video.jpeg_quality : ipcam_param_get_jpeg_quality();
    /* 状态响应从同一控制器读取，避免把编译期配置当作实际生效值。 */
    char buf[1024];
    int m = snprintf(buf, sizeof(buf),
                 "{\"ring_count\":%d,"
                 "\"model\":\"%s\",\"swver\":\"%s\","
                 "\"net_mode\":%u,\"capture_w\":%u,\"capture_h\":%u,"
                  "\"target_fps\":%u,\"capture_fps\":%u,\"output_fps\":%u,"
                  "\"actual_fps\":%u,\"jpeg_q\":%u,\"http_port\":%u,"
                  "\"preview_enabled\":%u,\"mirror_h\":%u,\"mirror_v\":%u,"
                  "\"backlight_percent\":%u,\"light_percent\":%u,"
                  "\"record_state\":%d,\"record_segment\":%u,"
                  "\"config_generation\":%u,\"network_link\":%d,"
                  "\"network_ip\":\"%s\",\"storage_mounted\":%u,"
                  "\"storage_available\":%llu,"
                  "\"jpeg_live_dropped\":%llu,\"jpeg_record_dropped\":%llu,"
                  "\"capture_frames\":%llu,\"capture_drop_display\":%llu,"
                  "\"capture_drop_encode\":%llu}\n",
                 cnt,
                 ipcam_param_get_model(), ipcam_param_get_swver(),
                 ipcam_param_get_net_mode(), status_w, status_h,
                 status_target_fps, status_capture_fps, status_output_fps,
                 status_capture_fps, status_q, ipcam_param_get_http_port(),
                 ipcam_param_get_preview_enabled(), ipcam_param_get_mirror_horizontal(),
                 ipcam_param_get_mirror_vertical(),
                 have_control ? cst.backlight_percent : ipcam_param_get_backlight_percent(),
                 have_control ? cst.light_percent : 0,
                 (int)rst.state, rst.segment_no,
                 have_control ? cst.config_generation : ipcam_param_get_generation(),
                 have_control ? cst.network_link : 0,
                 have_control ? cst.network_ip : "",
                 have_control ? cst.storage_mounted : 0,
                 (unsigned long long)(have_control ? cst.storage_available_bytes : 0),
                 (unsigned long long)(have_control ? cst.jpeg_live_dropped : 0),
                 (unsigned long long)(have_control ? cst.jpeg_record_dropped : 0),
                 (unsigned long long)(have_control ? cst.capture_frames : 0),
                 (unsigned long long)(have_control ? cst.capture_dropped_display : 0),
                 (unsigned long long)(have_control ? cst.capture_dropped_encode : 0));
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
/* 解析配置请求；媒体字段必须进入统一控制队列，网络字段保留旧语义。 */
static void serve_config_post(int fd, const char *body, size_t body_len,
                              ipcam_record_ctx_t *rec,
                              ipcam_display_ctx_t *display,
                              ipcam_screen_ctx_t *screen,
                              ipcam_control_ctx_t *control)
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
        if (rec && (!strcmp(key, "capture_w") || !strcmp(key, "capture_h") ||
                    !strcmp(key, "target_fps") || !strcmp(key, "mirror_horizontal") ||
                    !strcmp(key, "mirror_vertical"))) {
            ipcam_record_status_t rst;
            ipcam_record_get_status(rec, &rst);
            if (rst.state == IPCAM_RECORD_STARTING || rst.state == IPCAM_RECORD_RECORDING ||
                rst.state == IPCAM_RECORD_STOPPING) {
                MLOGW("config_post: %s rejected while recording\n", key);
                errors++;
                continue;
            }
        }
        /* 媒体相关字段统一走控制队列；网络字段仍沿用原有参数入口。 */
        if (control && (!strcmp(key, "capture_w") || !strcmp(key, "capture_h") ||
                        !strcmp(key, "target_fps") || !strcmp(key, "jpeg_quality") ||
                        !strcmp(key, "mirror_horizontal") || !strcmp(key, "mirror_vertical") ||
                        !strcmp(key, "preview_enabled") || !strcmp(key, "backlight_percent") ||
                        !strcmp(key, "screen_timeout_min"))) {
            ipcam_control_command_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            if (!strcmp(key, "capture_w")) {
                cmd.type = IPCAM_CONTROL_SET_VIDEO;
                cmd.video.width = (uint16_t)atoi(val);
                cmd.video.height = ipcam_param_get_capture_h();
                cmd.video.target_fps = ipcam_param_get_target_fps();
                cmd.video.jpeg_quality = ipcam_param_get_jpeg_quality();
            } else if (!strcmp(key, "capture_h")) {
                cmd.type = IPCAM_CONTROL_SET_VIDEO;
                cmd.video.width = ipcam_param_get_capture_w();
                cmd.video.height = (uint16_t)atoi(val);
                cmd.video.target_fps = ipcam_param_get_target_fps();
                cmd.video.jpeg_quality = ipcam_param_get_jpeg_quality();
            } else if (!strcmp(key, "target_fps") || !strcmp(key, "jpeg_quality")) {
                cmd.type = IPCAM_CONTROL_SET_VIDEO;
                cmd.video.width = ipcam_param_get_capture_w();
                cmd.video.height = ipcam_param_get_capture_h();
                cmd.video.target_fps = !strcmp(key, "target_fps") ?
                                       (uint8_t)atoi(val) : ipcam_param_get_target_fps();
                cmd.video.jpeg_quality = !strcmp(key, "jpeg_quality") ?
                                         (uint8_t)atoi(val) : ipcam_param_get_jpeg_quality();
            } else if (!strcmp(key, "mirror_horizontal") || !strcmp(key, "mirror_vertical")) {
                cmd.type = IPCAM_CONTROL_SET_MIRROR;
                cmd.mirror_horizontal = !strcmp(key, "mirror_horizontal") ?
                                        atoi(val) : ipcam_param_get_mirror_horizontal();
                cmd.mirror_vertical = !strcmp(key, "mirror_vertical") ?
                                      atoi(val) : ipcam_param_get_mirror_vertical();
            } else if (!strcmp(key, "preview_enabled")) {
                cmd.type = IPCAM_CONTROL_SET_PREVIEW; cmd.enabled = atoi(val) ? 1 : 0;
            } else if (!strcmp(key, "backlight_percent")) {
                cmd.type = IPCAM_CONTROL_SET_BACKLIGHT; cmd.percent = atoi(val);
            } else {
                cmd.type = IPCAM_CONTROL_SET_SCREEN_TIMEOUT; cmd.timeout_min = atoi(val);
            }
            uint64_t request_id = 0;
            ipcam_control_result_t result;
            memset(&result, 0, sizeof(result));
            rc = ipcam_control_submit_command(control, &cmd, &request_id);
            if (rc == 0 && ipcam_control_get_command_result(control, request_id, &result) == 0)
                rc = result.state == IPCAM_CONTROL_RESULT_DONE ? 0 : -1;
            if (rc == 0) {
                MLOGI("config_post: %s = %s (control request=%llu)\n",
                      key, val, (unsigned long long)request_id);
                changes++;
            } else {
                MLOGW("config_post: %s = %s rejected by control queue\n", key, val);
                errors++;
            }
            continue;
        }
        if      (!strcmp(key, "wifi_ssid"))    rc = ipcam_param_set_wifi_ssid(val);
        else if (!strcmp(key, "wifi_psk"))     rc = ipcam_param_set_wifi_psk(val);
        else if (!strcmp(key, "apn"))          rc = ipcam_param_set_apn(val);
        else if (!strcmp(key, "net_mode"))     rc = ipcam_param_set_net_mode((uint8_t)atoi(val));
        else if (!strcmp(key, "capture_w"))    rc = ipcam_param_set_capture_w((uint16_t)atoi(val));
        else if (!strcmp(key, "capture_h"))    rc = ipcam_param_set_capture_h((uint16_t)atoi(val));
        else if (!strcmp(key, "jpeg_quality")) rc = ipcam_param_set_jpeg_quality((uint8_t)atoi(val));
        else if (!strcmp(key, "target_fps"))   rc = ipcam_param_set_target_fps((uint8_t)atoi(val));
        else if (!strcmp(key, "http_port"))    rc = ipcam_param_set_http_port((uint16_t)atoi(val));
        else if (!strcmp(key, "log_level"))    rc = ipcam_param_set_log_level((uint8_t)atoi(val));
        else if (!strcmp(key, "http_bind_local")) rc = ipcam_param_set_http_bind_local((uint8_t)atoi(val));
        else if (!strcmp(key, "mirror_horizontal")) rc = ipcam_param_set_mirror_horizontal((uint8_t)atoi(val));
        else if (!strcmp(key, "mirror_vertical")) rc = ipcam_param_set_mirror_vertical((uint8_t)atoi(val));
        else if (!strcmp(key, "preview_enabled")) rc = ipcam_param_set_preview_enabled((uint8_t)atoi(val));
        else if (!strcmp(key, "backlight_percent")) rc = ipcam_param_set_backlight_percent((uint8_t)atoi(val));
        else if (!strcmp(key, "screen_timeout_min")) rc = ipcam_param_set_screen_timeout_min((uint8_t)atoi(val));
        else { MLOGW("config_post: unknown key '%s'\n", key); errors++; continue; }

        if (rc == 0 && !strcmp(key, "backlight_percent") && display &&
            ipcam_display_set_backlight(display, (int)atoi(val)) != 0) {
            MLOGW("config_post: backlight hardware rejected\n");
            rc = -1;
        }
        if (rc == 0 && screen && !strcmp(key, "backlight_percent"))
            ipcam_screen_update(screen, (int)atoi(val), ipcam_param_get_screen_timeout_min());
        if (rc == 0 && screen && !strcmp(key, "screen_timeout_min"))
            ipcam_screen_update(screen, ipcam_param_get_backlight_percent(), (int)atoi(val));

        if (rc == 0) {
            MLOGI("config_post: %s = %s (saved)\n", key, val);
            changes++;
        } else {
            MLOGW("config_post: %s = %s rejected\n", key, val);
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

/* 输出实测能力；硬件节点缺失时明确给出 touch/backlight=false。 */
static void serve_capabilities(int fd, ipcam_control_ctx_t *control)
{
    ipcam_control_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    if (!control || ipcam_control_get_capabilities(control, &caps) != 0) {
        const char *e = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, e, strlen(e));
        return;
    }
    char body[512];
    int n = snprintf(body, sizeof(body),
        "{\"video\":[{\"width\":%u,\"height\":%u,\"fps\":[%u]}],"
        "\"jpeg_quality\":{\"min\":%u,\"max\":%u},"
        "\"mirror\":{\"horizontal\":%s,\"vertical\":%s},"
        "\"preview_zoom\":{\"min\":%.1f,\"max\":%.1f},"
        "\"record\":{\"format\":\"mjpeg-avi\",\"segment_seconds\":%u},"
        "\"touch_points\":%u,\"backlight\":%s,\"light\":%s}\n",
        caps.video[0].width, caps.video[0].height, caps.video[0].target_fps,
        caps.jpeg_quality_min, caps.jpeg_quality_max,
        caps.mirror_horizontal ? "true" : "false", caps.mirror_vertical ? "true" : "false",
        caps.preview_zoom_min, caps.preview_zoom_max, caps.record_segment_seconds,
        caps.touch_points, caps.backlight ? "true" : "false",
        caps.light ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(body)) return;
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    safe_write(fd, hdr, (size_t)hn); safe_write(fd, body, (size_t)n);
}

/* 返回录像状态机、分段、重复帧与收尾错误。 */
static void serve_record_status(int fd, ipcam_record_ctx_t *rec,
                                ipcam_control_ctx_t *control)
{
    ipcam_record_status_t st;
    memset(&st, 0, sizeof(st));
    if (rec) ipcam_record_get_status(rec, &st);
    if (control) {
        ipcam_control_status_t cst;
        if (ipcam_control_get_status(control, &cst) == 0) st = cst.record;
    }
    char body[640];
    int n = snprintf(body, sizeof(body),
        "{\"state\":%d,\"segment_no\":%u,\"frame_count\":%llu,"
        "\"repeated_frames\":%llu,\"bytes_written\":%llu,"
        "\"elapsed_ms\":%llu,\"current_file\":\"%s\",\"error\":\"%s\"}\n",
        (int)st.state, st.segment_no,
        (unsigned long long)st.frame_count,
        (unsigned long long)st.repeated_frames,
        (unsigned long long)st.bytes_written,
        (unsigned long long)st.elapsed_ms,
        st.current_file, st.last_error);
    if (n < 0 || n >= (int)sizeof(body)) n = 0;
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    safe_write(fd, hdr, (size_t)hn); safe_write(fd, body, (size_t)n);
}

/* 将 start/stop 转为统一控制命令，返回 request_id 供异步状态查询。 */
static void serve_record_post(int fd, ipcam_record_ctx_t *rec,
                              ipcam_control_ctx_t *control,
                              const char *body, size_t body_len)
{
    if (!rec) {
        const char *m = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, m, strlen(m)); return;
    }
    char *work = strndup(body, body_len);
    if (!work) return;
    char action[32] = "";
    for (char *p = strtok(work, "&"); p; p = strtok(NULL, "&")) {
        char *eq = strchr(p, '='); if (!eq) continue; *eq = '\0';
        if (!strcmp(p, "action")) snprintf(action, sizeof(action), "%s", eq + 1);
    }
    int rc = -1;
    uint64_t request_id = 0;
    if (control) {
        ipcam_control_command_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = !strcmp(action, "start") ? IPCAM_CONTROL_RECORD_START :
                   !strcmp(action, "stop") ? IPCAM_CONTROL_RECORD_STOP : 0;
        if (cmd.type) rc = ipcam_control_submit_command(control, &cmd, &request_id);
    } else {
        rc = !strcmp(action, "start") ? ipcam_record_request_start(rec) :
             !strcmp(action, "stop") ? ipcam_record_request_stop(rec) : -1;
    }
    free(work);
    const char *status = rc == 0 ? "200 OK" : "409 Conflict";
    char out[160]; int n = snprintf(out, sizeof(out), "{\"ok\":%s,\"request_id\":%llu}\n",
                                    rc == 0 ? "true" : "false",
                                    (unsigned long long)request_id);
    char hdr[256]; int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        status, n);
    safe_write(fd, hdr, (size_t)hn); safe_write(fd, out, (size_t)n);
}

/* 独立拍照命令不复用 snapshot 下载语义，完成 SD 落盘后返回路径。 */
static void serve_photo_post(int fd, ipcam_record_ctx_t *rec,
                             ipcam_control_ctx_t *control)
{
    char path[256] = "";
    int rc = -1;
    uint64_t request_id = 0;
    if (control) {
        ipcam_control_command_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = IPCAM_CONTROL_PHOTO;
        rc = ipcam_control_submit_command(control, &cmd, &request_id);
        if (rc == 0) {
            ipcam_control_result_t result;
            if (ipcam_control_get_command_result(control, request_id, &result) == 0) {
                snprintf(path, sizeof(path), "%s", result.path);
                rc = result.state == IPCAM_CONTROL_RESULT_DONE ? 0 : -1;
            }
        }
    } else {
        rc = rec ? ipcam_record_save_photo(rec, path, sizeof(path)) : -1;
    }
    char body[384];
    int n = snprintf(body, sizeof(body), "{\"ok\":%s,\"request_id\":%llu,\"path\":\"%s\"}\n",
                     rc == 0 ? "true" : "false",
                     (unsigned long long)request_id, path);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        rc == 0 ? "200 OK" : "409 Conflict", n);
    safe_write(fd, hdr, (size_t)hn); safe_write(fd, body, (size_t)n);
}

/* 解析 GUI/调试控制表单，并把媒体操作交给统一控制队列。 */
static void serve_control_post(int fd, ipcam_stream_ctx_t *ctx,
                               const char *body, size_t body_len)
{
    char *work = strndup(body, body_len);
    char command[32] = "";
    int enabled = -1;
    int mirror_h = -1, mirror_v = -1;
    float zoom = 1.0f, center_x = 0.5f, center_y = 0.5f;
    int percent = -1, timeout_min = -1;
    if (work) {
        for (char *p = strtok(work, "&"); p; p = strtok(NULL, "&")) {
            char *eq = strchr(p, '='); if (!eq) continue; *eq = '\0';
            if (!strcmp(p, "command")) snprintf(command, sizeof(command), "%s", eq + 1);
            else if (!strcmp(p, "enabled")) enabled = atoi(eq + 1) ? 1 : 0;
            else if (!strcmp(p, "mirror_horizontal")) mirror_h = atoi(eq + 1) ? 1 : 0;
            else if (!strcmp(p, "mirror_vertical")) mirror_v = atoi(eq + 1) ? 1 : 0;
            else if (!strcmp(p, "zoom")) zoom = (float)atof(eq + 1);
            else if (!strcmp(p, "center_x")) center_x = (float)atof(eq + 1);
            else if (!strcmp(p, "center_y")) center_y = (float)atof(eq + 1);
            else if (!strcmp(p, "percent")) percent = atoi(eq + 1);
            else if (!strcmp(p, "timeout_min")) timeout_min = atoi(eq + 1);
        }
        free(work);
    }
    int rc = -1;
    uint64_t request_id = 0;
    ipcam_control_result_t result;
    memset(&result, 0, sizeof(result));
    if (ctx->control) {
        ipcam_control_command_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        if (!strcmp(command, "preview") && enabled >= 0) {
            cmd.type = IPCAM_CONTROL_SET_PREVIEW;
            cmd.enabled = enabled;
        } else if (!strcmp(command, "view")) {
            cmd.type = IPCAM_CONTROL_SET_VIEW;
            cmd.enabled = enabled < 0 ? 1 : enabled;
            cmd.zoom = zoom; cmd.center_x = center_x; cmd.center_y = center_y;
        } else if (!strcmp(command, "mirror") && mirror_h >= 0 && mirror_v >= 0) {
            cmd.type = IPCAM_CONTROL_SET_MIRROR;
            cmd.mirror_horizontal = mirror_h; cmd.mirror_vertical = mirror_v;
        } else if (!strcmp(command, "backlight") && percent >= 0) {
            cmd.type = IPCAM_CONTROL_SET_BACKLIGHT; cmd.percent = percent;
        } else if (!strcmp(command, "light") && percent >= 0) {
            cmd.type = IPCAM_CONTROL_SET_LIGHT; cmd.percent = percent;
        } else if (!strcmp(command, "screen_timeout") && timeout_min >= 0) {
            cmd.type = IPCAM_CONTROL_SET_SCREEN_TIMEOUT; cmd.timeout_min = timeout_min;
        } else if (!strcmp(command, "record_start")) {
            cmd.type = IPCAM_CONTROL_RECORD_START;
        } else if (!strcmp(command, "record_stop")) {
            cmd.type = IPCAM_CONTROL_RECORD_STOP;
        } else if (!strcmp(command, "photo")) {
            cmd.type = IPCAM_CONTROL_PHOTO;
        }
        if (cmd.type) {
            rc = ipcam_control_submit_command(ctx->control, &cmd, &request_id);
            if (rc == 0 && ipcam_control_get_command_result(ctx->control, request_id, &result) == 0)
                rc = result.state == IPCAM_CONTROL_RESULT_DONE ? 0 : -1;
        }
    } else if (!strcmp(command, "preview") && ctx->display && enabled >= 0) {
        int old_enabled; float old_zoom, old_x, old_y;
        ipcam_display_get_view(ctx->display, &old_enabled, &old_zoom, &old_x, &old_y);
        rc = ipcam_param_set_preview_enabled((uint8_t)enabled);
        if (rc == 0) rc = ipcam_display_set_view(ctx->display, enabled, old_zoom, old_x, old_y);
    } else if (!strcmp(command, "view") && ctx->display) {
        rc = ipcam_display_set_view(ctx->display, enabled < 0 ? 1 : enabled,
                                    zoom, center_x, center_y);
    } else if (!strcmp(command, "record_start") && ctx->recorder) {
        rc = ipcam_record_request_start(ctx->recorder);
    } else if (!strcmp(command, "record_stop") && ctx->recorder) {
        rc = ipcam_record_request_stop(ctx->recorder);
    } else if (!strcmp(command, "photo") && ctx->recorder) {
        rc = ipcam_record_save_photo(ctx->recorder, NULL, 0);
    }
    char out[384]; int n = snprintf(out, sizeof(out),
        "{\"ok\":%s,\"request_id\":%llu,\"message\":\"%s\",\"path\":\"%s\"}\n",
        rc == 0 ? "true" : "false", (unsigned long long)request_id,
        result.message, result.path);
    char hdr[256]; int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        rc == 0 ? "200 OK" : "409 Conflict", n);
    safe_write(fd, hdr, (size_t)hn); safe_write(fd, out, (size_t)n);
}

/* 查询统一控制队列的请求结果：GET /api/control/result?id=<request_id>。 */
static void serve_control_result(int fd, ipcam_control_ctx_t *control,
                                 const char *path)
{
    const char *q = strchr(path, '?');
    uint64_t request_id = 0;
    if (q) {
        const char *id = strstr(q + 1, "id=");
        if (id) request_id = strtoull(id + 3, NULL, 10);
    }
    ipcam_control_result_t result;
    memset(&result, 0, sizeof(result));
    int rc = control ? ipcam_control_get_command_result(control, request_id, &result) : -1;
    if (rc != 0) {
        const char *m = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        safe_write(fd, m, strlen(m));
        return;
    }
    char body[512];
    int n = snprintf(body, sizeof(body),
        "{\"request_id\":%llu,\"state\":%d,\"persisted\":%d,"
        "\"error_code\":%d,\"message\":\"%s\",\"path\":\"%s\"}\n",
        (unsigned long long)result.request_id, (int)result.state,
        result.persisted, result.error_code, result.message, result.path);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    safe_write(fd, hdr, (size_t)hn); safe_write(fd, body, (size_t)n);
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
    (void)ring_mtx;
    /* 编码环槽上限目前由 main 按能力值分配；用 1 MiB 上限避免网络线程
     * 把 ring 槽指针带出生命周期，同时拒绝异常大的压缩帧。 */
    size_t local_cap = ipcam_ring_capacity(jpeg_rb);
    unsigned char *local = local_cap ? malloc(local_cap) : NULL;
    if (!local) {
        const char *e = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, e, strlen(e));
        return;
    }
    ipcam_frame_t f;
    if (ipcam_ring_copy_latest(jpeg_rb, local, local_cap, &f, 0) != 0) {
        free(local);
        const char *e = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
        safe_write(fd, e, strlen(e));
        return;
    }
    size_t frame_size = f.size;

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

static void serve_stream(int fd, unsigned long sid,
                         ipcam_ring_buffer_t *jpeg_rb, pthread_mutex_t *ring_mtx,
                         volatile sig_atomic_t *running,
                         volatile sig_atomic_t *service_running)
{
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=ipcam\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (safe_write(fd, hdr, strlen(hdr)) < 0) {
        MLOGW("stream header failed: sid=%lu fd=%d\n", sid, fd);
        return;
    }

    /* 每个客户端只复制“最新帧”，不消费共享 ring。
     * 这样慢客户端会自然跳帧，也不会阻塞其它客户端或编码线程。 */
    (void)ring_mtx;
    size_t local_cap = ipcam_ring_capacity(jpeg_rb);
    unsigned char *local = local_cap ? malloc(local_cap) : NULL;
    if (!local) {
        MLOGE("stream buffer allocation failed: sid=%lu capacity=%zu\n",
              sid, local_cap);
        return;
    }
    unsigned long last_seq = 0;
    unsigned long frames_sent = 0;

    /* 既要响应进程退出，也要响应只停止 HTTP 服务的局部生命周期；
     * 旧实现只检查 running，main 关闭直播时会因客户端仍在等待新帧而无法收敛。 */
    while (*running && (!service_running || *service_running)) {
        ipcam_frame_t f;
        int gr = ipcam_ring_copy_latest(jpeg_rb, local, local_cap, &f, last_seq);
        if (gr != 0) {
            usleep(gr < 0 ? 50 * 1000 : 10 * 1000);
            continue;
        }
        last_seq = f.seqNo;
        size_t frame_size = f.size;

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
        frames_sent++;
        /* 慢客户端只取最新帧；按首批/周期帧记录发送序号，能直接判断网络
         * 卡顿还是编码端没有产生新 JPEG，同时不会按 15 fps 刷屏。 */
        if (frames_sent <= 3 || (frames_sent % 30) == 0) {
            MLOGI("stream frame: sid=%lu no=%lu seq=%lu bytes=%zu\n",
                  sid, frames_sent, f.seqNo, frame_size);
        }
        continue;
cleanup:
        MLOGI("stream session drain: sid=%lu frames=%lu last_seq=%lu\n",
              sid, frames_sent, last_seq);
        free(local);
        return;
    }

    MLOGI("stream session drain: sid=%lu frames=%lu last_seq=%lu reason=service_stop\n",
          sid, frames_sent, last_seq);
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

/*
 * === /healthz (GET) → 用于 init.d 回滚看门狗 ===
 *
 * 进程还活着不等于媒体链路仍能产出；watchdog 需要把共享 JPEG ring
 * 已关闭这一终态报告为失败，避免编码线程退出后设备继续被判定为健康。
 */
static void serve_healthz(int fd, ipcam_stream_ctx_t *ctx)
{
    int running = ctx && ctx->running && *ctx->running;
    int closed = !ctx || !ctx->jpeg_rb || ipcam_ring_is_closed(ctx->jpeg_rb);
    int queue_count = ctx && ctx->jpeg_rb ? ipcam_ring_count(ctx->jpeg_rb) : 0;
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
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        ok ? "200 OK" : "503 Service Unavailable", body_len);
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

    MLOGI("api POST /api/ota url=%s sha256=%s\n", url, sha);
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
        MLOGI("stream session start sid=%lu path=%s\n", sid, path);
        serve_stream(fd, sid, jpeg_rb, &ctx->ring_mtx, ctx->running,
                     &ctx->service_running);
        MLOGI("stream session end sid=%lu\n", sid);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/snapshot.jpg") == 0) {
        serve_snapshot(fd, jpeg_rb, &ctx->ring_mtx);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        serve_status(fd, jpeg_rb, ctx->recorder, ctx->control);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/capabilities") == 0) {
        serve_capabilities(fd, ctx->control);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/record") == 0) {
        serve_record_status(fd, ctx->recorder, ctx->control);
    } else if (strcmp(method, "GET") == 0 &&
               strncmp(path, "/api/control/result", strlen("/api/control/result")) == 0 &&
               (path[strlen("/api/control/result")] == '\0' ||
                path[strlen("/api/control/result")] == '?')) {
        serve_control_result(fd, ctx->control, path);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/config") == 0) {
        MLOGI("api GET /api/config\n");
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
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/photo") == 0) {
        serve_photo_post(fd, ctx->recorder, ctx->control);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/control") == 0) {
        int content_len = get_content_length(req);
        char *body = NULL;
        if (read_http_body(fd, req, req_len, content_len, &body) != 0) {
            const char *m = "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n";
            safe_write(fd, m, strlen(m));
        } else {
            serve_control_post(fd, ctx, body, (size_t)content_len);
            free(body);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/config") == 0) {
        int content_len = get_content_length(req);
        char *body = NULL;
        if (read_http_body(fd, req, req_len, content_len, &body) != 0) {
            const char *m = (content_len <= 0 || content_len > 4096)
                ? "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n"
                : "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            safe_write(fd, m, strlen(m));
        } else {
            MLOGI("api POST /api/config (%d bytes)\n", content_len);
            serve_config_post(fd, body, (size_t)content_len, ctx->recorder, ctx->display,
                              ctx->screen, ctx->control);
            free(body);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/record") == 0) {
        int content_len = get_content_length(req);
        char *body = NULL;
        if (read_http_body(fd, req, req_len, content_len, &body) != 0) {
            const char *m = "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n";
            safe_write(fd, m, strlen(m));
        } else {
            serve_record_post(fd, ctx->recorder, ctx->control, body, (size_t)content_len);
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

    /* fd 的最终 close 由拥有它的客户端线程执行；stop 侧只 shutdown，
     * 避免 fd 号码被复用后出现双重 close。 */
    close(a->cfd);

    pthread_mutex_lock(&ctx->client_mtx);
    int active_clients;
    if (a->slot >= 0 && a->slot < IPCAM_MAX_TRACKED_CLIENTS)
        ctx->client_fds[a->slot] = -1;
    if (ctx->client_cnt > 0) ctx->client_cnt--;
    active_clients = ctx->client_cnt;
    pthread_cond_broadcast(&ctx->client_cond);
    pthread_mutex_unlock(&ctx->client_mtx);

    MLOGI("client disconnected fd=%d slot=%d active=%d\n",
          a->cfd, a->slot, active_clients);

    free(a);
    return NULL;
}

static int acquire_client_slot(ipcam_stream_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->client_mtx);
    int max_clients = IPCAM_MAX_TRACKED_CLIENTS;  /* 受 client_fds[] 固定槽位容量限制 */
    const char *e = getenv("IPCAM_HTTP_MAX_CLIENTS");
    if (e && *e) {
        int v = atoi(e);
        if (v > 0 && v <= IPCAM_MAX_TRACKED_CLIENTS) max_clients = v;
    }
    int slot = -1;
    if (ctx->client_cnt < max_clients) {
        for (int i = 0; i < IPCAM_MAX_TRACKED_CLIENTS; i++) {
            if (ctx->client_fds[i] == -1) {
                /* -2 表示槽位已预留但真实 fd 尚未写入，避免 stop
                 * 与 accept_loop 在交接窗口重复使用同一槽位。 */
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

/* 线程创建失败或客户端尚未启动时释放预留槽位；-2 槽位没有 fd 可关闭。 */
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

    while (*ctx->running && ctx->service_running) {
        struct sockaddr_in cli_addr;
        socklen_t addrlen = sizeof(cli_addr);
        int cfd = accept(ctx->listen_fd, (struct sockaddr *)&cli_addr, &addrlen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!*ctx->running || !ctx->service_running) break;
            MLOGE("accept: %s\n", strerror(errno));
            usleep(100 * 1000);
            continue;
        }

        int slot = acquire_client_slot(ctx);
        if (slot < 0) {
            char ipbuf[32];
            inet_ntop(AF_INET, &cli_addr.sin_addr, ipbuf, sizeof(ipbuf));
            MLOGW("reject client %s:%d (max reached)\n",
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
        MLOGI("client %s:%d connected (active=%d)\n",
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
            MLOGE("pthread_create client\n");
            free(a);
            release_client_slot(ctx, slot);
            continue;
        }
        /* 分离客户端线程，结束后通过条件变量通知 stop 侧完成排空。 */
        pthread_detach(t);
    }
    return NULL;
}

/* 创建监听线程；control 在创建前注入，避免首个 HTTP 客户端绕过控制队列。 */
int ipcam_stream_start_ex(ipcam_stream_ctx_t *ctx, ipcam_ring_buffer_t *jpeg_rb,
                          volatile sig_atomic_t *running,
                          ipcam_control_ctx_t *control)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->jpeg_rb = jpeg_rb;
    ctx->control = control;
    /* 启动 accept 线程前复制可选服务引用，避免首个客户端抢在 main 的
     * setter 之后访问时看到空 recorder/display。后续 setter 仅用于兼容旧调用方。 */
    if (control) {
        ctx->recorder = control->recorder;
        ctx->display = control->display;
        ctx->screen = control->screen;
    }
    ctx->running = running;
    ctx->service_running = 1;
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
    /*
     * client_fds[] 的空槽统一用 -1 表示；当前并发上限由 client_cnt 维护，
     * 但仍需把槽位设为明确的无效 fd，避免后续回收逻辑把 0 误当成连接。
     */
    for (int i = 0; i < IPCAM_MAX_TRACKED_CLIENTS; i++) ctx->client_fds[i] = -1;

    /* 默认绑 127.0.0.1；IPCAM_HTTP_BIND 可覆盖（"0.0.0.0" 暴露给全网） */
    const char *bind_ip = getenv("IPCAM_HTTP_BIND");
    if (!bind_ip || !*bind_ip) {
        bind_ip = ipcam_param_get_http_bind_local() ? "127.0.0.1" : "0.0.0.0";
    }

    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        MLOGE("socket: %s\n", strerror(errno));
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
        MLOGE("invalid bind ip: %s\n", bind_ip);
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        stream_destroy_sync(ctx);
        return -1;
    }
    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        MLOGE("bind %s:%d: %s\n", bind_ip, ctx->port, strerror(errno));
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        stream_destroy_sync(ctx);
        return -1;
    }
    if (listen(ctx->listen_fd, BACKLOG) < 0) {
        MLOGE("listen: %s\n", strerror(errno));
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        stream_destroy_sync(ctx);
        return -1;
    }
    MLOGI("HTTP MJPEG server listening on %s:%d (max_clients=%d)\n",
          bind_ip, ctx->port, IPCAM_MAX_TRACKED_CLIENTS);

    if (pthread_create(&ctx->thread, NULL, accept_loop, ctx) != 0) {
        MLOGE("pthread_create accept\n");
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
        stream_destroy_sync(ctx);
        return -1;
    }
    MLOGI("HTTP service ready: bind=%s port=%d\n", bind_ip, ctx->port);
    return 0;
}

/* 兼容旧调用方：不注入控制器时仍可只启动直播。 */
int ipcam_stream_start(ipcam_stream_ctx_t *ctx, ipcam_ring_buffer_t *jpeg_rb,
                       volatile sig_atomic_t *running)
{
    return ipcam_stream_start_ex(ctx, jpeg_rb, running, NULL);
}

/* 兼容旧启动流程注入录像服务；HTTP 线程只读取该引用，不接管录像生命周期。 */
void ipcam_stream_set_recorder(ipcam_stream_ctx_t *ctx, ipcam_record_ctx_t *recorder)
{
    if (ctx) ctx->recorder = recorder;
}

/* HTTP 只保存控制器引用，实际校验与状态变更统一交给控制队列。 */
void ipcam_stream_set_control(ipcam_stream_ctx_t *ctx, ipcam_control_ctx_t *control)
{
    if (ctx) ctx->control = control;
}

/* 注入本地显示引用，供视口和预览状态查询；不会直接操作 framebuffer。 */
void ipcam_stream_set_display(ipcam_stream_ctx_t *ctx, ipcam_display_ctx_t *display)
{
    if (ctx) ctx->display = display;
}

/* 注入熄屏服务引用，配置入口可据此同步背光计时状态。 */
void ipcam_stream_set_screen(ipcam_stream_ctx_t *ctx, ipcam_screen_ctx_t *screen)
{
    if (ctx) ctx->screen = screen;
}

void ipcam_stream_stop(ipcam_stream_ctx_t *ctx)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->client_mtx);
    int clients_before_stop = ctx->client_cnt;
    pthread_mutex_unlock(&ctx->client_mtx);
    MLOGI("HTTP stop requested: clients=%d\n", clients_before_stop);
    /* 只关闭 HTTP 服务；采集、编码和录像由各自的生命周期管理。 */
    ctx->service_running = 0;
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

    /* 共享 JPEG ring 由 main cleanup 统一关闭；HTTP stop 只唤醒自身网络 I/O，
     * 避免停止直播服务时连带阻断编码、预览或其它 ring 观察者。 */
    pthread_mutex_lock(&ctx->client_mtx);
    for (int i = 0; i < IPCAM_MAX_TRACKED_CLIENTS; i++) {
        if (ctx->client_fds[i] >= 0) shutdown(ctx->client_fds[i], SHUT_RDWR);
    }
    while (ctx->client_cnt > 0) {
        pthread_cond_wait(&ctx->client_cond, &ctx->client_mtx);
    }
    pthread_mutex_unlock(&ctx->client_mtx);

    stream_destroy_sync(ctx);
    MLOGI("HTTP stopped: clients=0\n");
}
