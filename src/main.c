#define _GNU_SOURCE
#include "ipcam_capture.h"
#include "ipcam_cli.h"
#include "ipcam_display.h"
#include "ipcam_encode.h"
#include "ipcam_log.h"
#include "ipcam_net4g.h"
#include "ipcam_netwifi.h"
#include "ipcam_ota.h"
#include "ipcam_param.h"
#include "ipcam_ringbuffer.h"
#include "ipcam_stream.h"
#include "ipcam_sys.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ipcam_config.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

typedef struct {
    int net_started;
    int net_is_4g;
    int capture_started;
    int display_started;
    int encode_started;
    int stream_started;

    ipcam_net_4g_ctx_t  net4g;
    ipcam_net_wifi_ctx_t netwf;

    ipcam_capture_ctx_t  cap;
    ipcam_display_ctx_t  dis;
    ipcam_encode_ctx_t   enc;
    ipcam_stream_ctx_t   http;

    ipcam_ring_buffer_t *rb_yuyv_disp;
    ipcam_ring_buffer_t *rb_yuyv_enc;
    ipcam_ring_buffer_t *rb_jpeg;
} subsys_t;

static void cleanup_all(subsys_t *s)
{
    if (s->rb_yuyv_disp) ipcam_ring_close(s->rb_yuyv_disp);
    if (s->rb_yuyv_enc)  ipcam_ring_close(s->rb_yuyv_enc);
    if (s->rb_jpeg)      ipcam_ring_close(s->rb_jpeg);

    if (s->stream_started)  { MLOGI("stopping stream_http\n");   ipcam_stream_stop(&s->http);  s->stream_started = 0; }
    if (s->encode_started)  { MLOGI("stopping encode\n");        ipcam_encode_stop(&s->enc);   s->encode_started = 0; }
    if (s->display_started) { MLOGI("stopping display\n");       ipcam_display_stop(&s->dis);  s->display_started = 0; }
    if (s->capture_started) { MLOGI("stopping capture\n");       ipcam_capture_stop(&s->cap);  s->capture_started = 0; }

    if (s->net_started) {
        if (s->net_is_4g) ipcam_net_4g_stop(&s->net4g);
        else              ipcam_net_wifi_stop(&s->netwf);
        s->net_started = 0;
    }

    if (s->rb_yuyv_disp) { ipcam_ring_destroy(s->rb_yuyv_disp); s->rb_yuyv_disp = NULL; }
    if (s->rb_yuyv_enc)  { ipcam_ring_destroy(s->rb_yuyv_enc);  s->rb_yuyv_enc  = NULL; }
    if (s->rb_jpeg)      { ipcam_ring_destroy(s->rb_jpeg);      s->rb_jpeg      = NULL; }
}

static int run_daemon(void)
{
    /* BCF2 风格：sys_init 最早（log 注册、崩溃 handler） */
    ipcam_sys_init(IPCAM_MODEL);
    ipcam_sys_register_crash_handlers();
    ipcam_sys_print_banner();
    ipcam_sys_print_lib_versions();

    /* 加载运行时参数（覆盖 compile-time 默认） */
    const char *param_path = getenv("IPCAM_PARAM_PATH");
    ipcam_param_init(param_path ? param_path : IPCAM_PARAM_PATH_DEF);
    ipcam_param_dump();

    /* OTA 模块（检查 ELF + 当前版本） */
    ipcam_ota_init(IPCAM_OTA_PATH_DEF);

    /* 应用运行时 log level */
    uint8_t ll = ipcam_param_get_log_level();
    if (ll < IPCAM_LOG_BUTT) {
        ipcam_log_setlevel((ipcam_log_level_t)ll);
    }

    /* 信号处理：SIGINT/SIGTERM 优雅退出；SIGPIPE 忽略 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sigpipe_sa;
    memset(&sigpipe_sa, 0, sizeof(sigpipe_sa));
    sigpipe_sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sigpipe_sa, NULL);

    subsys_t s;
    memset(&s, 0, sizeof(s));

    /* 1) 网络层（读 param 决定模式） */
    uint8_t nmode = ipcam_param_get_net_mode();
    s.net_is_4g = (nmode == IPCAM_NET_MODE_4G);
    if (nmode == IPCAM_NET_MODE_4G || nmode == IPCAM_NET_MODE_WIFI) {
        if (s.net_is_4g) {
            ipcam_net_4g_init(&s.net4g, ipcam_param_get_apn(), "/dev/ttyUSB2");
            if (ipcam_net_4g_start(&s.net4g) == 0) s.net_started = 1;
            else MLOGW("4G start failed, continuing in local-only mode\n");
        } else {
            ipcam_net_wifi_init(&s.netwf, ipcam_param_get_wifi_ssid(),
                                ipcam_param_get_wifi_psk(), "wlan0");
            if (ipcam_net_wifi_start(&s.netwf) == 0) s.net_started = 1;
            else MLOGW("WiFi start failed, continuing in local-only mode\n");
        }
    } else {
        MLOGI("net mode = none; running locally only\n");
    }

    /* 2) 环形缓冲（按 param 决定尺寸） */
    uint16_t cap_w_cfg = ipcam_param_get_capture_w();
    uint16_t cap_h_cfg = ipcam_param_get_capture_h();
    size_t yuyv_bytes = (size_t)cap_w_cfg * cap_h_cfg * 2;
    s.rb_yuyv_disp = ipcam_ring_create(IPCAM_RING_DEPTH, yuyv_bytes);
    s.rb_yuyv_enc  = ipcam_ring_create(IPCAM_RING_DEPTH, yuyv_bytes);
    if (!s.rb_yuyv_disp || !s.rb_yuyv_enc) {
        MLOGE("alloc yuyv ring buffer(s) failed\n");
        cleanup_all(&s);
        return 1;
    }

    size_t jpeg_bytes = 256 * 1024;
    s.rb_jpeg = ipcam_ring_create(IPCAM_RING_DEPTH, jpeg_bytes);
    if (!s.rb_jpeg) {
        MLOGE("alloc jpeg ring buffer failed\n");
        cleanup_all(&s);
        return 1;
    }

    /* 3) 启动线程（顺序：capture -> display -> encode -> stream） */
    if (ipcam_capture_start(&s.cap, s.rb_yuyv_disp, s.rb_yuyv_enc, &g_running) < 0) {
        MLOGE("capture start failed\n");
        cleanup_all(&s);
        return 1;
    }
    s.capture_started = 1;

    int cap_w = cap_w_cfg, cap_h = cap_h_cfg;
    ipcam_capture_get_dimensions(&s.cap, &cap_w, &cap_h);
    MLOGI("capture final dims: %dx%d\n", cap_w, cap_h);

    if (ipcam_display_start(&s.dis, s.rb_yuyv_disp, cap_w, cap_h, &g_running) < 0) {
        MLOGE("display start failed\n");
        cleanup_all(&s);
        return 1;
    }
    s.display_started = 1;

    if (ipcam_encode_start(&s.enc, s.rb_yuyv_enc, s.rb_jpeg, cap_w, cap_h, &g_running) < 0) {
        MLOGE("encode start failed\n");
        cleanup_all(&s);
        return 1;
    }
    s.encode_started = 1;

    /* display-only 模式（encode stub 设置 quality=0 sentinel）：跳过 stream */
    int encode_is_stub = (s.enc.quality == 0);

    if (!encode_is_stub) {
        if (ipcam_stream_start(&s.http, s.rb_jpeg, &g_running) < 0) {
            MLOGE("http stream start failed\n");
            cleanup_all(&s);
            return 1;
        }
        s.stream_started = 1;
    } else {
        MLOGW("encode stub detected (display-only build); skipping HTTP stream\n");
    }

    MLOGI("ipcam running. Visit http://<board_ip>:%d/ in a browser.\n",
          ipcam_param_get_http_port());
    while (g_running) {
        sleep(1);
    }
    MLOGI("=== shutting down ===\n");

    cleanup_all(&s);
    MLOGI("=== ipcam exited cleanly ===\n");
    return 0;
}

int main(int argc, char **argv)
{
    int rc = ipcam_cli_dispatch(argc, argv);
    if (rc >= 0) return rc;
    return run_daemon();
}