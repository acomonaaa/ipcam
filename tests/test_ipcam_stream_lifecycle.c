#define _GNU_SOURCE

#include "ipcam_log.h"
#include "ipcam_ota.h"
#include "ipcam_param.h"
#include "ipcam_ringbuffer.h"
#include "ipcam_stream.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * 测试只验证 stream/ring 的线程生命周期，不应依赖板端参数、OTA 或日志
 * 后端；这些最小桩让测试可以在主机上直接链接真实业务实现。
 */
void ipcam_log_printf(ipcam_log_level_t level, const char *module,
                      const char *file, uint32_t line, const char *fmt, ...)
{
    (void)level;
    (void)module;
    (void)file;
    (void)line;
    (void)fmt;
}

static char g_empty[] = "";
static char g_model[] = "test-ipcam";
static char g_swver[] = "test";

uint8_t ipcam_param_get_net_mode(void) { return IPCAM_NET_MODE_NONE; }
const char *ipcam_param_get_wifi_ssid(void) { return g_empty; }
const char *ipcam_param_get_wifi_psk(void) { return g_empty; }
const char *ipcam_param_get_apn(void) { return g_empty; }
uint16_t ipcam_param_get_capture_w(void) { return 640; }
uint16_t ipcam_param_get_capture_h(void) { return 480; }
uint16_t ipcam_param_get_out_w(void) { return 0; }
uint16_t ipcam_param_get_out_h(void) { return 0; }
uint8_t ipcam_param_get_jpeg_quality(void) { return 75; }
uint8_t ipcam_param_get_target_fps(void) { return 15; }
uint16_t ipcam_param_get_http_port(void) { return 0; }
uint8_t ipcam_param_get_http_bind_local(void) { return 1; }
uint8_t ipcam_param_get_log_level(void) { return IPCAM_LOG_INFO; }
const char *ipcam_param_get_model(void) { return g_model; }
const char *ipcam_param_get_swver(void) { return g_swver; }

int ipcam_param_set_net_mode(uint8_t value) { (void)value; return 0; }
int ipcam_param_set_wifi_ssid(const char *value) { (void)value; return 0; }
int ipcam_param_set_wifi_psk(const char *value) { (void)value; return 0; }
int ipcam_param_set_apn(const char *value) { (void)value; return 0; }
int ipcam_param_set_capture_w(uint16_t value) { (void)value; return 0; }
int ipcam_param_set_capture_h(uint16_t value) { (void)value; return 0; }
int ipcam_param_set_out_w(uint16_t value) { (void)value; return 0; }
int ipcam_param_set_out_h(uint16_t value) { (void)value; return 0; }
int ipcam_param_set_jpeg_quality(uint8_t value) { (void)value; return 0; }
int ipcam_param_set_target_fps(uint8_t value) { (void)value; return 0; }
int ipcam_param_set_http_port(uint16_t value) { (void)value; return 0; }
int ipcam_param_set_http_bind_local(uint8_t value) { (void)value; return 0; }
int ipcam_param_set_log_level(uint8_t value) { (void)value; return 0; }

int ipcam_param_to_json(char *buf, size_t buf_sz)
{
    if (!buf || buf_sz < 3) return -1;
    memcpy(buf, "{}", 3);
    return 2;
}

void ipcam_ota_get_status(ipcam_ota_result_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

int ipcam_ota_from_url(const char *url, const char *sha256,
                       ipcam_ota_result_t *out)
{
    (void)url;
    (void)sha256;
    if (out) memset(out, 0, sizeof(*out));
    return -1;
}

static int connect_retry(int port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        close(fd);
        usleep(10 * 1000);
    }
    return -1;
}

static void send_request(int fd, const char *path)
{
    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                     path);
    assert(n > 0 && (size_t)n < sizeof(req));
    assert(write(fd, req, (size_t)n) == n);
}

static void assert_health_response(int fd, const char *status,
                                   const char *state)
{
    char response[512] = { 0 };
    size_t got = 0;
    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    while (got + 1 < sizeof(response)) {
        ssize_t n = read(fd, response + got, sizeof(response) - got - 1);
        if (n <= 0) break;
        got += (size_t)n;
        response[got] = '\0';
        if (strstr(response, "\r\n\r\n") && strstr(response, state)) break;
    }
    assert(strstr(response, status) != NULL);
    assert(strstr(response, state) != NULL);
}

static int current_port(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    assert(getsockname(listen_fd, (struct sockaddr *)&addr, &len) == 0);
    return ntohs(addr.sin_port);
}

static void wait_for_clients(ipcam_stream_ctx_t *ctx, int expected)
{
    for (int i = 0; i < 100; i++) {
        if (ctx->client_cnt >= expected) return;
        usleep(10 * 1000);
    }
    assert(ctx->client_cnt >= expected);
}

static void test_stop_wakes_waiting_clients(void)
{
    volatile sig_atomic_t running = 1;
    ipcam_ring_buffer_t *rb = ipcam_ring_create(4, 256);
    assert(rb != NULL);

    ipcam_stream_ctx_t ctx;
    assert(ipcam_stream_start(&ctx, rb, &running) == 0);
    int port = current_port(ctx.listen_fd);

    int health_fd = connect_retry(port);
    assert(health_fd >= 0);
    send_request(health_fd, "/healthz");
    assert_health_response(health_fd, "200 OK", "\"running\":true");
    close(health_fd);

    int clients[3];
    for (size_t i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
        clients[i] = connect_retry(port);
        assert(clients[i] >= 0);
        send_request(clients[i], "/stream.mjpg");
    }
    wait_for_clients(&ctx, 3);

    /*
     * 三个客户端都在 ring_get 等待帧；stop 必须通过 ring_close 和
     * shutdown 同时唤醒它们，并在返回前把 client_cnt 降到零。
     */
    ipcam_stream_stop(&ctx);
    assert(ctx.listen_fd == -1);
    assert(ctx.client_cnt == 0);
    assert(ipcam_ring_is_closed(rb) == 1);
    for (size_t i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) close(clients[i]);
    ipcam_ring_destroy(rb);
}

static void test_healthz_reports_closed_output(void)
{
    volatile sig_atomic_t running = 1;
    ipcam_ring_buffer_t *rb = ipcam_ring_create(2, 128);
    assert(rb != NULL);

    ipcam_stream_ctx_t ctx;
    assert(ipcam_stream_start(&ctx, rb, &running) == 0);
    int port = current_port(ctx.listen_fd);

    int health_fd = connect_retry(port);
    assert(health_fd >= 0);
    send_request(health_fd, "/healthz");
    assert_health_response(health_fd, "200 OK", "\"ok\":true");
    close(health_fd);

    /*
     * 先关闭输出 ring，但保持进程运行，验证 healthz 能区分“进程还在”
     * 与“视频输出已经停止”；这样不依赖 accept loop 与 running=0 的调度竞态。
     */
    ipcam_ring_close(rb);
    health_fd = connect_retry(port);
    assert(health_fd >= 0);
    send_request(health_fd, "/healthz");
    assert_health_response(health_fd, "503 Service Unavailable", "\"ok\":false");
    close(health_fd);

    ipcam_stream_stop(&ctx);
    assert(ctx.client_cnt == 0);
    ipcam_ring_destroy(rb);
}

int main(void)
{
    /* 网络写端断开时只应让客户端线程收尾，不应终止测试进程。 */
    signal(SIGPIPE, SIG_IGN);
    test_stop_wakes_waiting_clients();
    test_healthz_reports_closed_output();
    puts("ipcam stream lifecycle tests: PASS");
    return 0;
}
