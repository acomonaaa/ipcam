#define _GNU_SOURCE
/* BCF2 按源文件区分日志模块；主编排日志统一标记为 MAIN。 */
#define IPCAM_LOG_MODULE "MAIN"
#include "ipcam_capture.h"
#include "ipcam_cli.h"
#include "ipcam_control.h"
#include "ipcam_display.h"
#include "ipcam_encode.h"
#include "ipcam_log.h"
#include "ipcam_light.h"
#include "ipcam_lvgl.h"
#include "ipcam_net4g.h"
#include "ipcam_netwifi.h"
#include "ipcam_ota.h"
#include "ipcam_param.h"
#include "ipcam_record.h"
#include "ipcam_ringbuffer.h"
#include "ipcam_stream.h"
#include "ipcam_screen.h"
#include "ipcam_touch.h"
#include "ipcam_sys.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/sysinfo.h>
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
    int record_started;
    int touch_started;
    int screen_started;
    int lvgl_started;
    int stream_started;
    int control_started;

    ipcam_net_4g_ctx_t  net4g;
    ipcam_net_wifi_ctx_t netwf;

    ipcam_capture_ctx_t  cap;
    ipcam_display_ctx_t  dis;
    ipcam_encode_ctx_t   enc;
    ipcam_record_ctx_t   rec;
    ipcam_touch_ctx_t    touch;
    ipcam_screen_ctx_t   screen;
    ipcam_lvgl_ctx_t     lvgl;
    ipcam_stream_ctx_t   http;
    ipcam_control_ctx_t  control;

    ipcam_ring_buffer_t *rb_yuyv_disp;
    ipcam_ring_buffer_t *rb_yuyv_enc;
    ipcam_ring_buffer_t *rb_jpeg;
    ipcam_ring_buffer_t *rb_jpeg_record;

    /* 触摸手势只修改本地视口；网络编码和录像不读取这组临时状态。 */
    struct {
        ipcam_screen_ctx_t *screen;
        ipcam_display_ctx_t *display;
        ipcam_lvgl_ctx_t *lvgl;
        int active;
        int id0;
        int id1;
        float start_distance;
        float start_mid_x;
        float start_mid_y;
        float start_zoom;
        float start_center_x;
        float start_center_y;
        int start_enabled;
    } touch_runtime;
} subsys_t;

/* 查找触点，保证 slot 重排时仍按 tracking_id 识别同一根手指。 */
static const ipcam_touch_point_t *find_touch_point(const ipcam_touch_point_t *points,
                                                   int count, int tracking_id)
{
    for (int i = 0; i < count; i++)
        if (points[i].active && points[i].tracking_id == tracking_id) return &points[i];
    return NULL;
}

/* 结束当前双指手势；任一手指抬起后不把剩余触点转换为按钮点击。 */
static void reset_touch_gesture(subsys_t *s)
{
    s->touch_runtime.active = 0;
}

/* 将 evdev 触点快照转换为双指缩放/平移；唤醒抑制和边界裁剪在此统一完成。 */
static void on_touch_report(const ipcam_touch_point_t *points, int count, void *opaque)
{
    subsys_t *s = opaque;
    if (!s) return;
    if (s->touch_runtime.screen) {
        ipcam_screen_touch(s->touch_runtime.screen, count);
        /* 熄屏唤醒的首个触摸只负责亮屏，不能误触发缩放或按钮动作。 */
        if (!ipcam_screen_accept_input(s->touch_runtime.screen)) return;
    }
    /* LVGL 只接收已经完成坐标归一化的快照，避免它再次读取同一个 evdev。 */
    if (s->touch_runtime.lvgl)
        ipcam_lvgl_touch_report(s->touch_runtime.lvgl, points, count);
    if (count < 2 || !s->touch_runtime.display) {
        reset_touch_gesture(s);
        return;
    }

    const ipcam_touch_point_t *p0 = NULL, *p1 = NULL;
    for (int i = 0; i < count && (!p0 || !p1); i++) {
        if (!points[i].active) continue;
        if (!p0) p0 = &points[i];
        else if (points[i].tracking_id != p0->tracking_id) p1 = &points[i];
    }
    if (!p0 || !p1) {
        reset_touch_gesture(s);
        return;
    }

    ipcam_display_ctx_t *display = s->touch_runtime.display;
    /* 当前固件把整个 LCD 作为视频区域；GUI 接入后可把区域边界换成其布局值。 */
    if (!s->touch_runtime.active) {
        if (p0->x < 0 || p0->y < 0 || p1->x < 0 || p1->y < 0 ||
            p0->x >= display->out_w || p1->x >= display->out_w ||
            p0->y >= display->out_h || p1->y >= display->out_h) {
            reset_touch_gesture(s);
            return;
        }
        int enabled;
        ipcam_display_get_view(display, &enabled,
                               &s->touch_runtime.start_zoom,
                               &s->touch_runtime.start_center_x,
                               &s->touch_runtime.start_center_y);
        float dx = (float)p1->x - p0->x;
        float dy = (float)p1->y - p0->y;
        s->touch_runtime.start_distance = sqrtf(dx * dx + dy * dy);
        if (s->touch_runtime.start_distance < 1.0f) return;
        s->touch_runtime.id0 = p0->tracking_id;
        s->touch_runtime.id1 = p1->tracking_id;
        s->touch_runtime.start_mid_x = ((float)p0->x + p1->x) * 0.5f;
        s->touch_runtime.start_mid_y = ((float)p0->y + p1->y) * 0.5f;
        s->touch_runtime.start_enabled = enabled;
        s->touch_runtime.active = 1;
        return;
    }

    p0 = find_touch_point(points, count, s->touch_runtime.id0);
    p1 = find_touch_point(points, count, s->touch_runtime.id1);
    if (!p0 || !p1) {
        reset_touch_gesture(s);
        return;
    }
    float dx = (float)p1->x - p0->x;
    float dy = (float)p1->y - p0->y;
    float distance = sqrtf(dx * dx + dy * dy);
    if (distance < 1.0f) return;
    float zoom = s->touch_runtime.start_zoom * distance / s->touch_runtime.start_distance;
    if (zoom < 1.0f) zoom = 1.0f;
    if (zoom > 4.0f) zoom = 4.0f;
    float center_x = s->touch_runtime.start_center_x -
                     ((((float)p0->x + p1->x) * 0.5f) - s->touch_runtime.start_mid_x) /
                     (float)(display->out_w > 0 ? display->out_w : 1);
    float center_y = s->touch_runtime.start_center_y -
                     ((((float)p0->y + p1->y) * 0.5f) - s->touch_runtime.start_mid_y) /
                     (float)(display->out_h > 0 ? display->out_h : 1);
    /* 与 display 的等比例裁剪保持同一观察窗口，LCD 比源图宽时允许垂直平移。 */
    float view_w = 1.0f / zoom;
    float view_h = 1.0f / zoom;
    float src_ratio = (float)(display->src_w > 0 ? display->src_w : 1) /
                      (float)(display->src_h > 0 ? display->src_h : 1);
    float dst_ratio = (float)(display->out_w > 0 ? display->out_w : 1) /
                      (float)(display->out_h > 0 ? display->out_h : 1);
    if (src_ratio > dst_ratio)
        view_w = view_h * dst_ratio / src_ratio;
    else if (src_ratio < dst_ratio)
        view_h = view_w * src_ratio / dst_ratio;
    float half_x = view_w * 0.5f;
    float half_y = view_h * 0.5f;
    if (center_x < half_x) center_x = half_x;
    if (center_x > 1.0f - half_x) center_x = 1.0f - half_x;
    if (center_y < half_y) center_y = half_y;
    if (center_y > 1.0f - half_y) center_y = 1.0f - half_y;
    ipcam_display_set_view(display, s->touch_runtime.start_enabled,
                           zoom, center_x, center_y);
}

/* 读取 Linux 进程驻留集大小，供 5 秒性能汇总使用；失败返回 0 而不阻塞媒体。 */
static uint64_t read_rss_bytes(void)
{
    FILE *fp = fopen("/proc/self/statm", "r");
    unsigned long total_pages = 0;
    unsigned long resident = 0;
    if (!fp || fscanf(fp, "%lu %lu", &total_pages, &resident) != 2) {
        if (fp) fclose(fp);
        return 0;
    }
    fclose(fp);
    long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? (uint64_t)resident * (uint64_t)page : 0;
}

/* 累计计数器只增不减；若服务重启导致回绕/清零，窗口仍按当前值重新计数。 */
static uint64_t metrics_delta(uint64_t current, uint64_t previous)
{
    return current >= previous ? current - previous : current;
}

/* 按依赖反向停止各服务；先让网络客户端退出，再释放其引用的 ring 和控制器。 */
static void cleanup_all(subsys_t *s)
{
    if (s->rb_yuyv_disp) ipcam_ring_close(s->rb_yuyv_disp);
    if (s->rb_yuyv_enc)  ipcam_ring_close(s->rb_yuyv_enc);
    if (s->rb_jpeg)      ipcam_ring_close(s->rb_jpeg);
    if (s->rb_jpeg_record) ipcam_ring_close(s->rb_jpeg_record);

    if (s->stream_started)  { MLOGI("stopping stream_http\n");   ipcam_stream_stop(&s->http);  s->stream_started = 0; }
    if (s->touch_started)   { MLOGI("stopping touch\n");         ipcam_touch_stop(&s->touch); s->touch_started = 0; }
    /* LVGL 线程会读取 control 的只读状态；必须先停 UI，再销毁 control。 */
    if (s->lvgl_started)    { MLOGI("stopping lvgl\n");          ipcam_lvgl_stop(&s->lvgl); s->lvgl_started = 0; }
    if (s->control_started) { MLOGI("stopping control\n");       ipcam_control_deinit(&s->control); s->control_started = 0; }
    if (s->record_started)  { MLOGI("stopping recorder\n");      ipcam_record_stop(&s->rec);  s->record_started = 0; }
    if (s->screen_started)  { MLOGI("stopping screen\n");        ipcam_screen_stop(&s->screen); s->screen_started = 0; }
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
    if (s->rb_jpeg_record) { ipcam_ring_destroy(s->rb_jpeg_record); s->rb_jpeg_record = NULL; }
}

/* 守护进程编排入口；任一板级可选能力失败时仍保留控制/状态服务。 */
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
            const char *at_dev = getenv("IPCAM_4G_AT_DEV");
            const char *ppp_peer = getenv("IPCAM_4G_PPP_PEER");
            if (!at_dev || !*at_dev) at_dev = IPCAM_4G_AT_DEV;
            if (!ppp_peer || !*ppp_peer) ppp_peer = IPCAM_4G_PPP_PEER;
            MLOGI("4G config at_dev=%s ppp_peer=%s\n", at_dev, ppp_peer);
            ipcam_net_4g_init(&s.net4g, ipcam_param_get_apn(), at_dev, ppp_peer);
            if (ipcam_net_4g_start(&s.net4g) == 0) {
                s.net_started = 1;
                MLOGI("4G service ready: at_dev=%s ppp_peer=%s\n",
                      s.net4g.at_dev, s.net4g.ppp_peer);
            }
            else MLOGW("4G start failed, continuing in local-only mode\n");
        } else {
            ipcam_net_wifi_init(&s.netwf, ipcam_param_get_wifi_ssid(),
                                ipcam_param_get_wifi_psk(), "wlan0");
            if (ipcam_net_wifi_start(&s.netwf) == 0) {
                s.net_started = 1;
                MLOGI("WiFi service ready: ifname=%s\n", s.netwf.ifname);
            }
            else MLOGW("WiFi start failed, continuing in local-only mode\n");
        }
    } else {
        MLOGI("net mode = none; running locally only\n");
    }

    /* 2) 环形缓冲（按 param 决定尺寸） */
    /* 开机补光默认关闭；只有显式配置了板级节点才尝试写入，节点失败
     * 只报告能力不可用，不影响采集、直播和录像服务启动。 */
    if (getenv("IPCAM_LIGHT_PATH") && ipcam_light_set_percent(0) != 0)
        MLOGW("light default-off failed; check IPCAM_LIGHT_PATH\n");

    uint16_t cap_w_cfg = ipcam_param_get_capture_w();
    uint16_t cap_h_cfg = ipcam_param_get_capture_h();
    size_t yuyv_bytes = (size_t)cap_w_cfg * cap_h_cfg * 2U +
                        IPCAM_CAPTURE_RING_EXTRA_BYTES;
    s.rb_yuyv_disp = ipcam_ring_create(IPCAM_DISPLAY_RING_DEPTH, yuyv_bytes);
    s.rb_yuyv_enc  = ipcam_ring_create(IPCAM_RING_DEPTH, yuyv_bytes);
    if (!s.rb_yuyv_disp || !s.rb_yuyv_enc) {
        MLOGE("alloc yuyv ring buffer(s) failed\n");
        cleanup_all(&s);
        return 1;
    }
    MLOGI("YUYV rings ready: depth=%d slot=%zu/%zu\n",
          IPCAM_DISPLAY_RING_DEPTH, ipcam_ring_capacity(s.rb_yuyv_disp),
          ipcam_ring_capacity(s.rb_yuyv_enc));

    /* JPEG 上限按原始 YUYV 尺寸估算并设最低 1 MiB，避免高质量/高分辨率
     * 帧超过旧的固定 256 KiB 槽后被静默丢弃。 */
    size_t jpeg_bytes = yuyv_bytes + 64 * 1024;
    if (jpeg_bytes < 1024 * 1024) jpeg_bytes = 1024 * 1024;
    s.rb_jpeg = ipcam_ring_create(IPCAM_RING_DEPTH, jpeg_bytes);
    s.rb_jpeg_record = ipcam_ring_create(IPCAM_RECORD_RING_DEPTH, jpeg_bytes);
    if (!s.rb_jpeg || !s.rb_jpeg_record) {
        MLOGE("alloc jpeg ring buffer failed\n");
        cleanup_all(&s);
        return 1;
    }
    MLOGI("JPEG rings ready: live_depth=%d record_depth=%d slot=%zu\n",
          IPCAM_RING_DEPTH, IPCAM_RECORD_RING_DEPTH,
          ipcam_ring_capacity(s.rb_jpeg));

    /* 3) 启动线程（顺序：capture -> display -> encode -> record -> stream）。
     * 采集或 LCD 失败时仍保留控制/状态/HTTP 服务，让上位机能看到具体故障，
     * 不把显示故障误扩大为直播和录像故障。 */
    int cap_w = cap_w_cfg, cap_h = cap_h_cfg;
    if (ipcam_capture_start(&s.cap, s.rb_yuyv_disp, s.rb_yuyv_enc, &g_running) < 0) {
        MLOGE("capture start failed; keep control/status services alive\n");
    } else {
        s.capture_started = 1;
        ipcam_capture_get_dimensions(&s.cap, &cap_w, &cap_h);
        MLOGI("capture final dims: %dx%d\n", cap_w, cap_h);

        const char *lvgl_env = getenv("IPCAM_LVGL");
        int lvgl_requested = IPCAM_LVGL_ENABLE;
        if (lvgl_env && *lvgl_env) lvgl_requested = strcmp(lvgl_env, "0") != 0;

        /* LVGL 接管 LCD 刷新时，display 线程仍保留预览帧生产能力，但不再
         * 与 LVGL 同时写 framebuffer，避免两个线程交错覆盖同一块显存。 */
        if (ipcam_display_start_ex(&s.dis, s.rb_yuyv_disp, cap_w, cap_h,
                                   lvgl_requested ? 0 : 1, &g_running) < 0) {
            MLOGW("display start failed; keep network/recording services alive\n");
        } else {
            s.display_started = 1;
            MLOGI("display pipeline started: lvgl_requested=%d\n", lvgl_requested);
            if (lvgl_requested) {
                if (ipcam_lvgl_start(&s.lvgl, &s.dis, &g_running) == 0) {
                    s.lvgl_started = 1;
                    MLOGI("LVGL display frontend ready\n");
                } else {
                    /* LVGL 初始化失败时恢复旧的直接 framebuffer 路径，保证
                     * 显示能力降级而不是让 LCD 停留在黑屏。 */
                    ipcam_display_set_framebuffer_writer(&s.dis, 1);
                    MLOGW("lvgl start failed; fallback to direct framebuffer writer\n");
                }
            }
        }

        if (s.display_started &&
            ipcam_screen_start(&s.screen, &g_running, &s.dis,
                               ipcam_param_get_backlight_percent(),
                               ipcam_param_get_screen_timeout_min()) == 0) {
            s.screen_started = 1;
            MLOGI("screen power service started\n");
        } else if (s.display_started) {
            MLOGW("screen power service start failed\n");
        }
    }

    /* 触摸设备默认采用板上实测的 Goodix event1；环境变量仍可覆盖，便于
     * 不同硬件变体或临时诊断。未配置/不存在时不阻塞视频服务。 */
    const char *touch_dev = getenv("IPCAM_TOUCH_DEV");
    if (!touch_dev || !*touch_dev) touch_dev = IPCAM_TOUCH_DEV;
    s.touch_runtime.screen = s.screen_started ? &s.screen : NULL;
    s.touch_runtime.display = s.display_started ? &s.dis : NULL;
    s.touch_runtime.lvgl = s.lvgl_started ? &s.lvgl : NULL;
    if (touch_dev && *touch_dev && s.touch_runtime.display &&
        (s.touch_runtime.screen || s.touch_runtime.lvgl) &&
        ipcam_touch_start_ex(&s.touch, touch_dev, s.dis.out_w, s.dis.out_h,
                             on_touch_report, &s) == 0) {
        s.touch_started = 1;
        MLOGI("touch started: %s\n", touch_dev);
    } else if (touch_dev && *touch_dev && s.touch_runtime.display &&
               (s.touch_runtime.screen || s.touch_runtime.lvgl)) {
        MLOGW("touch start failed: %s\n", touch_dev);
    }

    if (s.capture_started &&
        ipcam_encode_start_ex(&s.enc, s.rb_yuyv_enc, s.rb_jpeg,
                              s.rb_jpeg_record, cap_w, cap_h, &g_running) < 0) {
        MLOGE("encode start failed; keep status service alive\n");
    } else if (s.capture_started) {
        s.encode_started = 1;
        MLOGI("encode pipeline started\n");
    }

    /* display-only 模式（encode stub 设置 quality=0 sentinel）：跳过 stream */
    int encode_is_stub = s.encode_started && (s.enc.quality == 0);

    if (s.encode_started && !encode_is_stub) {
        const char *storage_root = getenv("IPCAM_STORAGE_ROOT");
        if (ipcam_record_start(&s.rec, s.rb_jpeg_record, &g_running,
                               storage_root ? storage_root : IPCAM_STORAGE_ROOT,
                               cap_w, cap_h, ipcam_param_get_target_fps()) < 0) {
            MLOGE("record service start failed; continue without recording\n");
        } else {
            s.record_started = 1;
            MLOGI("record pipeline started\n");
        }
    }

    /* 所有控制入口共用同一个契约；控制器不创建线程，退出时先于被引用服务销毁。 */
    if (ipcam_control_init(&s.control,
                           s.capture_started ? &s.cap : NULL,
                           s.record_started ? &s.rec : NULL,
                           s.display_started ? &s.dis : NULL,
                           s.screen_started ? &s.screen : NULL,
                           s.rb_jpeg, &g_running) < 0) {
        MLOGE("control service start failed\n");
        cleanup_all(&s);
        return 1;
    }
    s.control_started = 1;
    MLOGI("control pipeline started\n");
    if (s.encode_started) {
        ipcam_control_set_encoder(&s.control, &s.enc);
        /* setter 后立即刷新一次，让首个 /api/status 同时带配置/有效质量。 */
        ipcam_control_refresh_status(&s.control, 0);
    }
    if (s.lvgl_started)
        ipcam_lvgl_set_control(&s.lvgl, &s.control);

    if (!encode_is_stub) {
        if (ipcam_stream_start_ex(&s.http, s.rb_jpeg, &g_running, &s.control) < 0) {
            MLOGE("http stream start failed; local control remains available\n");
        } else {
            s.stream_started = 1;
            MLOGI("HTTP pipeline started\n");
            ipcam_stream_set_recorder(&s.http, s.record_started ? &s.rec : NULL);
            ipcam_stream_set_display(&s.http, s.display_started ? &s.dis : NULL);
            ipcam_stream_set_screen(&s.http, s.screen_started ? &s.screen : NULL);
        }
    } else {
        MLOGW("encode stub detected (display-only build); skipping HTTP stream\n");
    }

    MLOGI("ipcam running: http_port=%d capture=%dx%d@%u jpeg_q=%u preview=%u\n",
          ipcam_param_get_http_port(), cap_w, cap_h,
          ipcam_param_get_target_fps(), ipcam_param_get_jpeg_quality(),
          ipcam_param_get_preview_enabled());
    unsigned metrics_tick = 0;
    uint64_t prev_capture_frames = 0;
    uint64_t prev_encode_frames = 0;
    uint64_t prev_display_frames = 0;
    uint64_t prev_stream_frames = 0;
    uint64_t prev_record_real_frames = 0;
    uint64_t prev_record_repeat_frames = 0;
    int peak_disp = 0, peak_enc = 0, peak_live = 0, peak_record = 0;
    while (g_running) {
        sleep(1);
        /* UI 回调只入队，主线程在这里串行执行硬件/文件控制命令。 */
        if (s.lvgl_started) ipcam_lvgl_process_actions(&s.lvgl);
        if (s.control_started)
            ipcam_control_refresh_status(&s.control, 0);
        if (++metrics_tick >= 5) {
            ipcam_record_status_t rst;
            memset(&rst, 0, sizeof(rst));
            if (s.record_started) ipcam_record_get_status(&s.rec, &rst);
            ipcam_encode_perf_t ep;
            ipcam_display_perf_t dp;
            ipcam_stream_perf_t sp;
            ipcam_record_perf_t rp;
            ipcam_lvgl_perf_t lp;
            memset(&ep, 0, sizeof(ep));
            memset(&dp, 0, sizeof(dp));
            memset(&sp, 0, sizeof(sp));
            memset(&rp, 0, sizeof(rp));
            memset(&lp, 0, sizeof(lp));
            if (s.encode_started) ipcam_encode_get_perf(&s.enc, &ep);
            if (s.display_started) ipcam_display_get_perf(&s.dis, &dp);
            if (s.stream_started) ipcam_stream_get_perf(&s.http, &sp);
            if (s.record_started) ipcam_record_get_perf(&s.rec, &rp);
            if (s.lvgl_started) ipcam_lvgl_get_perf(&s.lvgl, &lp);
            int q_disp = ipcam_ring_count(s.rb_yuyv_disp);
            int q_enc = ipcam_ring_count(s.rb_yuyv_enc);
            int q_live = ipcam_ring_count(s.rb_jpeg);
            int q_record = ipcam_ring_count(s.rb_jpeg_record);
            if (q_disp > peak_disp) peak_disp = q_disp;
            if (q_enc > peak_enc) peak_enc = q_enc;
            if (q_live > peak_live) peak_live = q_live;
            if (q_record > peak_record) peak_record = q_record;
            uint64_t capture_frames = 0, drop_disp = 0, drop_enc = 0;
            if (s.capture_started)
                ipcam_capture_get_stats(&s.cap, &capture_frames, &drop_disp, &drop_enc);
            uint64_t capture_delta = metrics_delta(capture_frames, prev_capture_frames);
            prev_capture_frames = capture_frames;
            uint64_t encode_delta = metrics_delta(ep.frames_encoded, prev_encode_frames);
            uint64_t display_delta = metrics_delta(dp.frames_rendered, prev_display_frames);
            uint64_t stream_delta = metrics_delta(sp.frames_sent, prev_stream_frames);
            uint64_t record_real_delta = metrics_delta(rp.real_frames, prev_record_real_frames);
            uint64_t record_repeat_delta = metrics_delta(rp.repeated_frames,
                                                         prev_record_repeat_frames);
            prev_encode_frames = ep.frames_encoded;
            prev_display_frames = dp.frames_rendered;
            prev_stream_frames = sp.frames_sent;
            prev_record_real_frames = rp.real_frames;
            prev_record_repeat_frames = rp.repeated_frames;
            MLOGI("metrics capture_fps=%.1f encode_fps=%.1f preview_fps=%.1f "
                  "stream_fps=%.1f record_real_fps=%.1f record_repeat_fps=%.1f "
                  "capture_frames=%llu encode_frames=%llu preview_frames=%llu "
                  "stream_frames=%llu record_real=%llu record_repeat=%llu "
                  "stale_yuyv_display=%llu stale_yuyv_encode=%llu "
                  "live_overwrite=%llu record_queue_drop=%llu "
                  "drop_disp=%llu drop_enc=%llu encode_drop=%llu "
                  "yuv420_ns=%llu/%llu/%llu jpeg_ns=%llu/%llu/%llu "
                  "jpeg_bytes=%llu/%llu quality=%u/%u adaptive=%u "
                  "capture_to_encode_p95_ns=%llu capture_to_send_start_p95_ns=%llu "
                  "lvgl_handler_ns=%llu/%llu/%llu lvgl_video_ns=%llu/%llu/%llu "
                  "lvgl_sleep_ns=%llu/%llu/%llu sleep_fast=%d/%d "
                  "fb_mode=%s fb_page=%d pan_ns=%llu/%llu/%llu pan_fail=%llu "
                  "rss=%lluB q=%d/%d/%d/%d peak=%d/%d/%d/%d "
                  "record_state=%d record_frames=%llu record_bytes=%llu\n",
                  capture_delta / 5.0, encode_delta / 5.0, display_delta / 5.0,
                  stream_delta / 5.0, record_real_delta / 5.0,
                  record_repeat_delta / 5.0,
                  (unsigned long long)capture_frames,
                  (unsigned long long)ep.frames_encoded,
                  (unsigned long long)dp.frames_rendered,
                  (unsigned long long)sp.frames_sent,
                  (unsigned long long)rp.real_frames,
                  (unsigned long long)rp.repeated_frames,
                  (unsigned long long)dp.stale_input_frames,
                  (unsigned long long)ep.stale_input_frames,
                  (unsigned long long)ep.live_overwrites,
                  (unsigned long long)ep.record_drops,
                  (unsigned long long)drop_disp, (unsigned long long)drop_enc,
                  (unsigned long long)ep.frames_dropped,
                  (unsigned long long)ep.yuv420_avg_ns,
                  (unsigned long long)ep.yuv420_p95_ns,
                  (unsigned long long)ep.yuv420_max_ns,
                  (unsigned long long)ep.jpeg_avg_ns,
                  (unsigned long long)ep.jpeg_p95_ns,
                  (unsigned long long)ep.jpeg_max_ns,
                  (unsigned long long)ep.jpeg_avg_bytes,
                  (unsigned long long)ep.jpeg_max_bytes,
                  ep.configured_quality, ep.effective_quality, ep.adaptive_quality,
                  (unsigned long long)ep.capture_to_output_p95_ns,
                  (unsigned long long)sp.capture_to_send_start_p95_ns,
                  (unsigned long long)lp.handler_avg_ns,
                  (unsigned long long)lp.handler_p95_ns,
                  (unsigned long long)lp.handler_max_ns,
                  (unsigned long long)lp.video_avg_ns,
                  (unsigned long long)lp.video_p95_ns,
                  (unsigned long long)lp.video_max_ns,
                  (unsigned long long)lp.sleep_avg_ns,
                  (unsigned long long)lp.sleep_p95_ns,
                  (unsigned long long)lp.sleep_max_ns,
                  lp.sleep_fast_path, lp.sleep_fast_active,
                  dp.framebuffer_mode, dp.framebuffer_active_page,
                  (unsigned long long)dp.pan_avg_ns,
                  (unsigned long long)dp.pan_p95_ns,
                  (unsigned long long)dp.pan_max_ns,
                  (unsigned long long)dp.pan_failures,
                  (unsigned long long)read_rss_bytes(), q_disp, q_enc, q_live, q_record,
                  peak_disp, peak_enc, peak_live, peak_record,
                  (int)rst.state, (unsigned long long)rp.frame_count,
                  (unsigned long long)rp.bytes_written);
            metrics_tick = 0;
        }
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
