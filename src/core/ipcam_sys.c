#define _GNU_SOURCE
#include "ipcam_sys.h"
#include "ipcam_param.h"
#include "ipcam_config.h"

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 异步信号安全（async-signal-safe）的 param 快照。
 * 在 SIG_DFL 之前由正常上下文预先调用并缓存；crash handler 只读这份快照。
 */
static ipcam_param_t s_crash_snapshot;

void ipcam_sys_take_snapshot(void)
{
    /* 仅在正常上下文调用。可在 main 启动期调一次，之后按需更新。 */
    const ipcam_param_t *p = ipcam_param_get();
    if (p) s_crash_snapshot = *p;
}

/* 用 write() 直写 stderr（async-signal-safe） */
static int safe_write_str(const char *s, size_t len)
{
    ssize_t n = write(STDERR_FILENO, s, len);
    (void)n;
    return 0;
}

/*
 * Crash handler —— 只调用 async-signal-safe 函数：
 *   write / _exit / raise / backtrace / backtrace_symbols_fd
 *   pthread_mutex_lock / fprintf 都不允许。
 * 阻塞 fatal 信号防止 handler 内再次崩溃被立即杀死。
 */
static void crash_handler(int sig)
{
    sigset_t block_all;
    sigfillset(&block_all);
    sigprocmask(SIG_BLOCK, &block_all, NULL);

    char buf[256];
    int n;
    n = snprintf(buf, sizeof(buf), "\n\n*** FATAL: signal %d ***\n", sig);
    safe_write_str(buf, (size_t)n);

    void *bt[32];
    int nbt = backtrace(bt, 32);
    backtrace_symbols_fd(bt, nbt, STDERR_FILENO);

    /* 用快照（已预先在正常上下文拷贝），避免 pthread_mutex_lock */
    n = snprintf(buf, sizeof(buf),
        "--- ipcam param snapshot ---\n"
        "  model=%s swver=%s net_mode=%u\n"
        "  wifi_ssid=%s apn=%s\n"
        "  capture=%ux%u jpeg_q=%u target_fps=%u\n"
        "  http_port=%u http_bind_local=%u log_level=%u\n"
        "--- end ---\n",
        s_crash_snapshot.model, s_crash_snapshot.swver, s_crash_snapshot.net_mode,
        s_crash_snapshot.wifi_ssid, s_crash_snapshot.apn,
        s_crash_snapshot.capture_w, s_crash_snapshot.capture_h,
        s_crash_snapshot.jpeg_quality, s_crash_snapshot.target_fps,
        s_crash_snapshot.http_port, s_crash_snapshot.http_bind_local,
        s_crash_snapshot.log_level);
    safe_write_str(buf, (size_t)n);

    /* 恢复默认 handler 再 raise，让内核产生 core dump / 终止 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    raise(sig);
}

void ipcam_sys_register_crash_handlers(void)
{
    /* 先拍快照，避免 handler 内访问被锁住或释放的 param */
    ipcam_sys_take_snapshot();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    /* 屏蔽所有信号，防止 handler 内重入 fatal 信号被立即杀死 */
    sigfillset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

int ipcam_sys_init(const char *module)
{
    ipcam_log_init(module ? module : IPCAM_MODEL);
    return 0;
}

void ipcam_sys_print_banner(void)
{
    fprintf(stderr,
        "=========================================================\n"
        "  ipcam  model=%s  swver=%s  build=%s %s\n"
        "  param path: %s\n"
        "=========================================================\n",
        IPCAM_MODEL, IPCAM_VERSION, __DATE__, __TIME__,
        IPCAM_PARAM_PATH_DEF);
}

void ipcam_sys_print_lib_versions(void)
{
    LIBIPCAM_LIB_VER_FMT(LIBIPCAM_LOG);
    LIBIPCAM_LIB_VER_FMT(LIBIPCAM_PARAM);
    LIBIPCAM_LIB_VER_FMT(LIBIPCAM_RINGBUF);
    LIBIPCAM_LIB_VER_FMT(LIBIPCAM_CAPTURE);
    LIBIPCAM_LIB_VER_FMT(LIBIPCAM_DISPLAY);
    LIBIPCAM_LIB_VER_FMT(LIBIPCAM_ENCODE);
    LIBIPCAM_LIB_VER_FMT(LIBIPCAM_STREAM);
    LIBIPCAM_LIB_VER_FMT(LIBIPCAM_NET);
}