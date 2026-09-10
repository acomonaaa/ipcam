#define _GNU_SOURCE

/* 熄屏、休眠提示、唤醒和背光状态日志归入 SCRN 模块。 */
#define IPCAM_LOG_MODULE "SCRN"
#include "ipcam_screen.h"
#include "ipcam_display.h"
#include "ipcam_log.h"

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IPCAM_SCREEN_PROMPT_LEAD_SEC 10ULL

/* 统一使用单调时钟，防止用户修改系统时间导致休眠计时跳变。 */
static uint64_t screen_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL +
           (uint64_t)ts.tv_nsec;
}

/* 根据配置生成固定休眠截止点；timeout=0 表示不设置截止点。 */
static uint64_t screen_deadline(uint64_t start_ns, int timeout_min)
{
    if (timeout_min <= 0) return 0;
    uint64_t duration = (uint64_t)timeout_min * 60ULL * 1000000000ULL;
    return UINT64_MAX - start_ns < duration ? UINT64_MAX : start_ns + duration;
}

/* 执行一次真正的本地休眠；采集、编码、HTTP 和录像线程完全不在此路径中。 */
static int screen_apply_sleep(ipcam_screen_ctx_t *ctx, const char *reason)
{
    if (!ctx || !ctx->display) return -1;
    /* 先停止生成新的本地帧，再关闭背光，避免屏幕刚切黑仍有旧帧写入。 */
    int rc = ipcam_display_set_screen_paused(ctx->display, 1);
    if (ipcam_display_set_backlight(ctx->display, 0) != 0) rc = -1;
    MLOGI("screen sleep: reason=%s backlight=%s\n", reason ? reason : "unknown",
          rc == 0 ? "off" : "best-effort");
    return rc;
}

/* 恢复本地预览和用户设定的亮度；FBIOBLANK 回退也在 display 层统一处理。 */
static int screen_apply_wake(ipcam_screen_ctx_t *ctx, int brightness)
{
    if (!ctx || !ctx->display) return -1;
    int rc = ipcam_display_set_backlight(ctx->display, brightness);
    if (ipcam_display_set_screen_paused(ctx->display, 0) != 0) rc = -1;
    return rc;
}

/* 每秒检查提示和截止点；提示期间 deadline 固定，不因普通触摸而无限顺延。 */
static void *screen_thread(void *arg)
{
    ipcam_screen_ctx_t *ctx = arg;
    while (*ctx->running && ctx->service_running) {
        sleep(1);
        uint64_t now = screen_now_ns();
        int show_prompt = 0;
        int should_sleep = 0;
        int timeout = 0;
        pthread_mutex_lock(&ctx->mtx);
        timeout = ctx->timeout_min;
        if (timeout > 0 && !ctx->sleeping) {
            if (!ctx->sleep_deadline_ns)
                ctx->sleep_deadline_ns = screen_deadline(ctx->last_touch_ns, timeout);
            uint64_t deadline = ctx->sleep_deadline_ns;
            if (now < deadline && deadline - now <= IPCAM_SCREEN_PROMPT_LEAD_SEC * 1000000000ULL) {
                if (!ctx->sleep_prompt_visible) {
                    ctx->sleep_prompt_visible = 1;
                    show_prompt = 1;
                }
            }
            if (now >= deadline) {
                ctx->sleeping = 1;
                ctx->sleep_prompt_visible = 0;
                should_sleep = 1;
            }
        }
        pthread_mutex_unlock(&ctx->mtx);

        if (show_prompt)
            MLOGI("screen sleep prompt shown: timeout=%dmin lead=%llus\n", timeout,
                  (unsigned long long)IPCAM_SCREEN_PROMPT_LEAD_SEC);
        if (should_sleep) {
            /* 触摸可能在解锁后抢先唤醒；重新持锁确认状态并执行硬件动作，
             * 保证“已唤醒”不会被一个滞后的 timeout 操作再次关背光。 */
            pthread_mutex_lock(&ctx->mtx);
            if (ctx->sleeping)
                (void)screen_apply_sleep(ctx, "timeout");
            pthread_mutex_unlock(&ctx->mtx);
        }
    }
    return NULL;
}

/* 启动休眠状态机；timeout=0 表示永不自动休眠，也不显示提示。 */
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
    ctx->sleep_deadline_ns = screen_deadline(ctx->last_touch_ns, timeout_min);
    pthread_mutex_init(&ctx->mtx, NULL);
    if (pthread_create(&ctx->thread, NULL, screen_thread, ctx) != 0) {
        pthread_mutex_destroy(&ctx->mtx);
        return -1;
    }
    MLOGI("screen service ready: backlight=%d%% timeout=%dmin prompt_lead=%llus\n",
          ctx->brightness_percent, ctx->timeout_min,
          (unsigned long long)IPCAM_SCREEN_PROMPT_LEAD_SEC);
    return 0;
}

/* 提交触点数量；提示期间不改变固定 deadline，唤醒后的首个触摸只恢复屏幕。 */
void ipcam_screen_touch(ipcam_screen_ctx_t *ctx, int active_points)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->mtx);
    uint64_t now = screen_now_ns();
    if (active_points > 0 && ctx->sleeping) {
        ctx->sleeping = 0;
        ctx->sleep_prompt_visible = 0;
        ctx->suppress_until_release = 1;
        ctx->last_touch_ns = now;
        ctx->sleep_deadline_ns = screen_deadline(now, ctx->timeout_min);
        int brightness = ctx->brightness_percent;
        ipcam_display_ctx_t *display = ctx->display;
        pthread_mutex_unlock(&ctx->mtx);
        if (display) {
            (void)ipcam_display_set_backlight(display, brightness);
            (void)ipcam_display_set_screen_paused(display, 0);
        }
        MLOGI("screen wake by touch; first touch is suppressed\n");
        return;
    }
    /* 提示是需要明确选择的模态框，点击卡片空白处不应偷改自动截止点。 */
    if (!ctx->sleep_prompt_visible) ctx->last_touch_ns = now;
    if (active_points == 0) ctx->suppress_until_release = 0;
    pthread_mutex_unlock(&ctx->mtx);
}

/* 更新持久配置对应的运行时副本，并在已休眠时立即恢复显示。 */
void ipcam_screen_update(ipcam_screen_ctx_t *ctx, int brightness_percent, int timeout_min)
{
    if (!ctx) return;
    if (brightness_percent < 10 || brightness_percent > 100) return;
    if (timeout_min != 0 && timeout_min != 1 && timeout_min != 3 &&
        timeout_min != 5 && timeout_min != 10) return;
    pthread_mutex_lock(&ctx->mtx);
    int was_sleeping = ctx->sleeping;
    ctx->sleeping = 0;
    ctx->sleep_prompt_visible = 0;
    ctx->suppress_until_release = 0;
    ctx->brightness_percent = brightness_percent;
    ctx->timeout_min = timeout_min;
    ctx->last_touch_ns = screen_now_ns();
    ctx->sleep_deadline_ns = screen_deadline(ctx->last_touch_ns, timeout_min);
    pthread_mutex_unlock(&ctx->mtx);
    MLOGI("screen config updated: backlight=%d%% timeout=%dmin\n",
          brightness_percent, timeout_min);
    if (was_sleeping) (void)screen_apply_wake(ctx, brightness_percent);
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

/* 读取休眠状态快照；倒计时向上取整，确保 UI 不提前显示“0 秒”。 */
int ipcam_screen_get_status(ipcam_screen_ctx_t *ctx,
                            ipcam_screen_status_t *status)
{
    if (!ctx || !status) return -1;
    memset(status, 0, sizeof(*status));
    pthread_mutex_lock(&ctx->mtx);
    status->sleeping = ctx->sleeping;
    status->sleep_prompt_visible = ctx->sleep_prompt_visible;
    status->timeout_min = ctx->timeout_min;
    uint64_t now = screen_now_ns();
    if (ctx->sleep_prompt_visible && ctx->sleep_deadline_ns > now) {
        uint64_t remaining = ctx->sleep_deadline_ns - now;
        status->sleep_remaining_sec = (int)((remaining + 999999999ULL) / 1000000000ULL);
    }
    pthread_mutex_unlock(&ctx->mtx);
    return 0;
}

/* “立即休眠”按钮调用；重复调用保持幂等，避免快速点击触发多次 blank。 */
int ipcam_screen_request_sleep(ipcam_screen_ctx_t *ctx)
{
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->mtx);
    int already_sleeping = ctx->sleeping;
    ctx->sleeping = 1;
    ctx->sleep_prompt_visible = 0;
    ctx->sleep_deadline_ns = 0;
    /* 状态和硬件切黑放在同一把锁内，防止触摸唤醒插入两者之间。 */
    int rc = already_sleeping ? 0 : screen_apply_sleep(ctx, "user");
    pthread_mutex_unlock(&ctx->mtx);
    return rc;
}

/* “继续显示”按钮调用；清除提示并从当前时刻重新开始完整计时。 */
int ipcam_screen_keep_awake(ipcam_screen_ctx_t *ctx)
{
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->mtx);
    int was_sleeping = ctx->sleeping;
    int brightness = ctx->brightness_percent;
    uint64_t now = screen_now_ns();
    ctx->sleeping = 0;
    ctx->sleep_prompt_visible = 0;
    ctx->suppress_until_release = 0;
    ctx->last_touch_ns = now;
    ctx->sleep_deadline_ns = screen_deadline(now, ctx->timeout_min);
    pthread_mutex_unlock(&ctx->mtx);
    if (was_sleeping) return screen_apply_wake(ctx, brightness);
    MLOGI("screen sleep prompt dismissed; idle timer restarted\n");
    return 0;
}

/* 停止计时线程并解除显示暂停；退出时恢复原亮度，避免留下 blank 状态。 */
void ipcam_screen_stop(ipcam_screen_ctx_t *ctx)
{
    if (!ctx) return;
    MLOGI("screen stop requested\n");
    ctx->service_running = 0;
    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }
    pthread_mutex_lock(&ctx->mtx);
    int was_sleeping = ctx->sleeping;
    int brightness = ctx->brightness_percent;
    pthread_mutex_unlock(&ctx->mtx);
    if (ctx->display) {
        if (was_sleeping) (void)ipcam_display_set_backlight(ctx->display, brightness);
        (void)ipcam_display_set_screen_paused(ctx->display, 0);
    }
    pthread_mutex_destroy(&ctx->mtx);
    MLOGI("screen stopped\n");
}
