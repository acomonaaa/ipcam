#define _GNU_SOURCE

/* 熄屏、唤醒和背光状态日志归入 SCRN 模块。 */
#define IPCAM_LOG_MODULE "SCRN"
#include "ipcam_screen.h"
#include "ipcam_display.h"
#include "ipcam_log.h"

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 统一使用单调时钟，防止用户修改系统时间导致熄屏计时跳变。 */
static uint64_t screen_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* 每秒检查空闲时长；只改变本地显示/背光，绝不触碰采集、编码和录像。 */
static void *screen_thread(void *arg)
{
    ipcam_screen_ctx_t *ctx = arg;
    while (*ctx->running && ctx->service_running) {
        sleep(1);
        pthread_mutex_lock(&ctx->mtx);
        int timeout = ctx->timeout_min;
        int should_sleep = timeout > 0 && !ctx->sleeping &&
            screen_now_ns() - ctx->last_touch_ns >= (uint64_t)timeout * 60ULL * 1000000000ULL;
        if (should_sleep) ctx->sleeping = 1;
        pthread_mutex_unlock(&ctx->mtx);
        if (should_sleep) {
            /* 熄屏不仅关背光，还暂停本地 YUYV 转换；网络编码/录像不受影响。 */
            if (ctx->display) ipcam_display_set_screen_paused(ctx->display, 1);
            ipcam_display_set_backlight_percent(0);
            MLOGI("screen backlight off after %d minutes idle\n", timeout);
        }
    }
    return NULL;
}

/* 启动熄屏状态机；timeout=0 表示永不自动熄屏。 */
int ipcam_screen_start(ipcam_screen_ctx_t *ctx, volatile sig_atomic_t *running,
                       ipcam_display_ctx_t *display,
                       int brightness_percent, int timeout_min)
{
    if (!ctx || !running) return -1;
    if (brightness_percent < 10 || brightness_percent > 100) brightness_percent = 100;
    if (timeout_min != 0 && timeout_min != 1 && timeout_min != 3 &&
        timeout_min != 5 && timeout_min != 10) timeout_min = 3;
    memset(ctx, 0, sizeof(*ctx));
    ctx->running = running;
    ctx->display = display;
    ctx->service_running = 1;
    ctx->timeout_min = timeout_min;
    ctx->brightness_percent = brightness_percent;
    ctx->last_touch_ns = screen_now_ns();
    pthread_mutex_init(&ctx->mtx, NULL);
    if (pthread_create(&ctx->thread, NULL, screen_thread, ctx) != 0) {
        pthread_mutex_destroy(&ctx->mtx);
        return -1;
    }
    MLOGI("screen service ready: backlight=%d%% timeout=%dmin\n",
          ctx->brightness_percent, ctx->timeout_min);
    return 0;
}

/* 提交触点数量；唤醒后的第一次触摸只恢复背光，释放后才恢复按钮输入。 */
void ipcam_screen_touch(ipcam_screen_ctx_t *ctx, int active_points)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->mtx);
    ctx->last_touch_ns = screen_now_ns();
    if (active_points > 0 && ctx->sleeping) {
        ctx->sleeping = 0;
        ctx->suppress_until_release = 1;
        int brightness = ctx->brightness_percent;
        ipcam_display_ctx_t *display = ctx->display;
        pthread_mutex_unlock(&ctx->mtx);
        if (display) ipcam_display_set_screen_paused(display, 0);
        ipcam_display_set_backlight_percent(brightness);
        MLOGI("screen wake by touch; first touch is suppressed\n");
        return;
    }
    if (active_points == 0) ctx->suppress_until_release = 0;
    pthread_mutex_unlock(&ctx->mtx);
}

/* 更新持久配置对应的运行时副本，并在已熄屏时立即恢复显示。 */
void ipcam_screen_update(ipcam_screen_ctx_t *ctx, int brightness_percent, int timeout_min)
{
    if (!ctx) return;
    if (brightness_percent < 10 || brightness_percent > 100) return;
    if (timeout_min != 0 && timeout_min != 1 && timeout_min != 3 &&
        timeout_min != 5 && timeout_min != 10) return;
    pthread_mutex_lock(&ctx->mtx);
    int was_sleeping = ctx->sleeping;
    ctx->sleeping = 0;
    ctx->suppress_until_release = 0;
    ctx->brightness_percent = brightness_percent;
    ctx->timeout_min = timeout_min;
    ctx->last_touch_ns = screen_now_ns();
    pthread_mutex_unlock(&ctx->mtx);
    MLOGI("screen config updated: backlight=%d%% timeout=%dmin\n",
          brightness_percent, timeout_min);
    if (was_sleeping) {
        if (ctx->display) ipcam_display_set_screen_paused(ctx->display, 0);
        ipcam_display_set_backlight_percent(brightness_percent);
    }
}

/* 返回当前触摸是否可交给 GUI；唤醒抑制窗口期间返回 0。 */
int ipcam_screen_accept_input(ipcam_screen_ctx_t *ctx)
{
    if (!ctx) return 1;
    pthread_mutex_lock(&ctx->mtx);
    int accepted = !ctx->suppress_until_release;
    pthread_mutex_unlock(&ctx->mtx);
    return accepted;
}

/* 停止计时线程并解除显示暂停标志。 */
void ipcam_screen_stop(ipcam_screen_ctx_t *ctx)
{
    if (!ctx) return;
    MLOGI("screen stop requested\n");
    ctx->service_running = 0;
    if (ctx->thread) { pthread_join(ctx->thread, NULL); ctx->thread = 0; }
    if (ctx->display) ipcam_display_set_screen_paused(ctx->display, 0);
    pthread_mutex_destroy(&ctx->mtx);
    MLOGI("screen stopped\n");
}
