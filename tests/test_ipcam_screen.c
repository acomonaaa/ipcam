#define _GNU_SOURCE

#include "ipcam_log.h"
#include "ipcam_screen.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * 休眠状态机测试不打开真实 LCD；用最小 display 桩记录背光和预览暂停，
 * 通过公开状态/动作接口验证计时语义，避免测试需要等待真实的 1～10 分钟。
 */
static pthread_mutex_t g_display_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_last_brightness = -1;
static int g_last_paused = -1;

void ipcam_log_printf(ipcam_log_level_t level, const char *module,
                      const char *file, uint32_t line, const char *fmt, ...)
{
    (void)level;
    (void)module;
    (void)file;
    (void)line;
    (void)fmt;
}

int ipcam_display_set_backlight(ipcam_display_ctx_t *ctx, int percent)
{
    (void)ctx;
    pthread_mutex_lock(&g_display_mtx);
    g_last_brightness = percent;
    pthread_mutex_unlock(&g_display_mtx);
    return 0;
}

int ipcam_display_set_screen_paused(ipcam_display_ctx_t *ctx, int paused)
{
    (void)ctx;
    pthread_mutex_lock(&g_display_mtx);
    g_last_paused = paused ? 1 : 0;
    pthread_mutex_unlock(&g_display_mtx);
    return 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void reset_display_probe(void)
{
    pthread_mutex_lock(&g_display_mtx);
    g_last_brightness = -1;
    g_last_paused = -1;
    pthread_mutex_unlock(&g_display_mtx);
}

static void set_deadline(ipcam_screen_ctx_t *ctx, uint64_t deadline_ns)
{
    pthread_mutex_lock(&ctx->mtx);
    ctx->sleep_deadline_ns = deadline_ns;
    ctx->sleep_prompt_visible = 0;
    pthread_mutex_unlock(&ctx->mtx);
}

static int read_status(ipcam_screen_ctx_t *ctx, ipcam_screen_status_t *status)
{
    memset(status, 0, sizeof(*status));
    return ipcam_screen_get_status(ctx, status);
}

static void wait_for_prompt(ipcam_screen_ctx_t *ctx)
{
    ipcam_screen_status_t status;
    for (int i = 0; i < 30; i++) {
        assert(read_status(ctx, &status) == 0);
        if (status.sleep_prompt_visible) {
            assert(status.sleep_remaining_sec >= 1);
            assert(status.sleep_remaining_sec <= 10);
            return;
        }
        usleep(100 * 1000);
    }
    assert(!"screen sleep prompt did not appear");
}

static void wait_for_sleep(ipcam_screen_ctx_t *ctx)
{
    ipcam_screen_status_t status;
    for (int i = 0; i < 30; i++) {
        assert(read_status(ctx, &status) == 0);
        if (status.sleeping) return;
        usleep(100 * 1000);
    }
    assert(!"screen did not enter sleep");
}

static void test_prompt_and_keep_awake(void)
{
    volatile sig_atomic_t running = 1;
    ipcam_display_ctx_t display;
    memset(&display, 0, sizeof(display));
    ipcam_screen_ctx_t screen;
    reset_display_probe();

    assert(ipcam_screen_start(&screen, &running, &display, 70, 1) == 0);
    /* 将截止点放入提示窗口，测试无需真实等待 50 秒。 */
    set_deadline(&screen, monotonic_ns() + 9ULL * 1000000000ULL);
    wait_for_prompt(&screen);

    assert(ipcam_screen_keep_awake(&screen) == 0);
    ipcam_screen_status_t status;
    assert(read_status(&screen, &status) == 0);
    assert(status.sleep_prompt_visible == 0);
    assert(status.sleeping == 0);
    pthread_mutex_lock(&screen.mtx);
    assert(screen.sleep_deadline_ns > monotonic_ns());
    pthread_mutex_unlock(&screen.mtx);

    ipcam_screen_stop(&screen);
}

static void test_auto_sleep_and_touch_wake(void)
{
    volatile sig_atomic_t running = 1;
    ipcam_display_ctx_t display;
    memset(&display, 0, sizeof(display));
    ipcam_screen_ctx_t screen;
    reset_display_probe();

    assert(ipcam_screen_start(&screen, &running, &display, 70, 1) == 0);
    set_deadline(&screen, monotonic_ns() - 1);
    wait_for_sleep(&screen);

    pthread_mutex_lock(&g_display_mtx);
    assert(g_last_paused == 1);
    assert(g_last_brightness == 0);
    pthread_mutex_unlock(&g_display_mtx);

    /* 唤醒首触摸只恢复显示；直到 release 才允许 GUI 收到输入。 */
    ipcam_screen_touch(&screen, 1);
    assert(ipcam_screen_accept_input(&screen) == 0);
    ipcam_screen_touch(&screen, 0);
    assert(ipcam_screen_accept_input(&screen) == 1);
    assert(read_status(&screen, &(ipcam_screen_status_t){0}) == 0);

    pthread_mutex_lock(&g_display_mtx);
    assert(g_last_paused == 0);
    assert(g_last_brightness == 70);
    pthread_mutex_unlock(&g_display_mtx);
    ipcam_screen_stop(&screen);
}

static void test_immediate_sleep_and_timeout_zero(void)
{
    volatile sig_atomic_t running = 1;
    ipcam_display_ctx_t display;
    memset(&display, 0, sizeof(display));
    ipcam_screen_ctx_t screen;
    reset_display_probe();

    assert(ipcam_screen_start(&screen, &running, &display, 80, 0) == 0);
    usleep(1200 * 1000);
    ipcam_screen_status_t status;
    assert(read_status(&screen, &status) == 0);
    assert(status.sleep_prompt_visible == 0);
    assert(status.sleeping == 0);

    assert(ipcam_screen_request_sleep(&screen) == 0);
    assert(read_status(&screen, &status) == 0);
    assert(status.sleeping == 1);
    /* 重复调用必须幂等，然后“继续显示”恢复正常计时。 */
    assert(ipcam_screen_request_sleep(&screen) == 0);
    assert(ipcam_screen_keep_awake(&screen) == 0);
    assert(read_status(&screen, &status) == 0);
    assert(status.sleeping == 0);
    assert(status.sleep_prompt_visible == 0);
    ipcam_screen_stop(&screen);
}

int main(void)
{
    test_prompt_and_keep_awake();
    test_auto_sleep_and_touch_wake();
    test_immediate_sleep_and_timeout_zero();
    puts("ipcam screen state tests: PASS");
    return 0;
}
