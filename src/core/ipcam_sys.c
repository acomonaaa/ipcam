#define _GNU_SOURCE
/* 系统横幅、崩溃快照和库版本日志归入 SYS 模块。 */
#define IPCAM_LOG_MODULE "SYS "
#include "ipcam_sys.h"
#include "ipcam_param.h"
#include "ipcam_config.h"

#include <execinfo.h>
#include <errno.h>
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

typedef struct ipcam_cpu_policy_s {
    char governor_path[128];
    char max_path[128];
    char old_governor[32];
    char old_max[32];
    int governor_saved;
    int max_saved;
    int changed;
    int restore_registered;
} ipcam_cpu_policy_t;

static ipcam_cpu_policy_t s_cpu_policy;

/* 读取 cpufreq 文本节点并去掉换行，便于保存后在退出时原样恢复。 */
static int cpu_read_text(const char *path, char *buf, size_t buf_sz)
{
    if (!path || !buf || buf_sz < 2) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buf, (int)buf_sz, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf[0] ? 0 : -1;
}

/* 写入 cpufreq 节点；失败只影响性能调优，不允许阻止摄像头服务启动。 */
static int cpu_write_text(const char *path, const char *value)
{
    if (!path || !value || !*value) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int ok = fputs(value, fp) >= 0;
    if (ok) ok = fputc('\n', fp) != EOF;
    int close_rc = fclose(fp);
    return ok && close_rc == 0 ? 0 : -1;
}

/* 正常退出时恢复 cpufreq，避免 ipcam 退出后悄悄改变系统默认电源策略。 */
static void ipcam_sys_restore_cpu_policy(void)
{
    if (!s_cpu_policy.changed) return;
    if (s_cpu_policy.max_saved)
        (void)cpu_write_text(s_cpu_policy.max_path, s_cpu_policy.old_max);
    if (s_cpu_policy.governor_saved)
        (void)cpu_write_text(s_cpu_policy.governor_path, s_cpu_policy.old_governor);
    s_cpu_policy.changed = 0;
}

/* 启动期尽力解除 powersave 限制；cpufreq 缺失或权限不足时保持原策略运行。 */
static void ipcam_sys_tune_cpu_policy(void)
{
    const char *desired = getenv("IPCAM_CPU_GOVERNOR");
    if (desired && !strcmp(desired, "0")) {
        MLOGI("CPU governor tuning disabled by IPCAM_CPU_GOVERNOR=0\n");
        return;
    }
    if (!desired || !*desired) desired = "performance";

    snprintf(s_cpu_policy.governor_path, sizeof(s_cpu_policy.governor_path),
             "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    snprintf(s_cpu_policy.max_path, sizeof(s_cpu_policy.max_path),
             "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
    if (cpu_read_text(s_cpu_policy.governor_path, s_cpu_policy.old_governor,
                      sizeof(s_cpu_policy.old_governor)) != 0) {
        MLOGW("CPU cpufreq unavailable: %s\n", strerror(errno));
        return;
    }
    s_cpu_policy.governor_saved = 1;
    if (cpu_read_text(s_cpu_policy.max_path, s_cpu_policy.old_max,
                      sizeof(s_cpu_policy.old_max)) == 0)
        s_cpu_policy.max_saved = 1;

    int changed = 0;
    if (strcmp(s_cpu_policy.old_governor, desired) != 0) {
        if (cpu_write_text(s_cpu_policy.governor_path, desired) == 0) {
            changed = 1;
            MLOGI("CPU governor: %s -> %s\n", s_cpu_policy.old_governor, desired);
        } else {
            MLOGW("CPU governor write failed: path=%s errno=%d(%s)\n",
                  s_cpu_policy.governor_path, errno, strerror(errno));
        }
    }

    const char *max_env = getenv("IPCAM_CPU_MAX_FREQ");
    char max_freq[32];
    if (max_env && *max_env) {
        snprintf(max_freq, sizeof(max_freq), "%s", max_env);
    } else if (cpu_read_text(
                   "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
                   max_freq, sizeof(max_freq)) != 0) {
        max_freq[0] = '\0';
    }
    if (s_cpu_policy.max_saved && max_freq[0] &&
        strcmp(s_cpu_policy.old_max, max_freq) != 0) {
        if (cpu_write_text(s_cpu_policy.max_path, max_freq) == 0) {
            changed = 1;
            MLOGI("CPU max frequency: %s -> %s kHz\n",
                  s_cpu_policy.old_max, max_freq);
        } else {
            MLOGW("CPU max frequency write failed: path=%s errno=%d(%s)\n",
                  s_cpu_policy.max_path, errno, strerror(errno));
        }
    }
    s_cpu_policy.changed = changed;
    if (changed && !s_cpu_policy.restore_registered) {
        if (atexit(ipcam_sys_restore_cpu_policy) == 0)
            s_cpu_policy.restore_registered = 1;
    }

    char current_freq[32];
    if (cpu_read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
                      current_freq, sizeof(current_freq)) == 0)
        MLOGI("CPU policy ready: governor=%s cur_freq=%s kHz\n", desired,
              current_freq);
    else
        MLOGI("CPU policy ready: governor=%s current frequency unavailable\n", desired);
}

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
    /* 崩溃路径只读取预先复制的结构，新增控制字段也放进快照，便于定位现场状态。 */
    n = snprintf(buf, sizeof(buf),
        "--- ipcam param snapshot ---\n"
        "  model=%s swver=%s net_mode=%u\n"
        "  wifi_ssid=%s apn=%s\n"
        "  capture=%ux%u jpeg_q=%u target_fps=%u\n"
        "  http_port=%u http_bind_local=%u log_level=%u\n"
        "  mirror_h=%u mirror_v=%u preview=%u backlight=%u timeout=%u\n"
        "--- end ---\n",
        s_crash_snapshot.model, s_crash_snapshot.swver, s_crash_snapshot.net_mode,
        s_crash_snapshot.wifi_ssid, s_crash_snapshot.apn,
        s_crash_snapshot.capture_w, s_crash_snapshot.capture_h,
        s_crash_snapshot.jpeg_quality, s_crash_snapshot.target_fps,
        s_crash_snapshot.http_port, s_crash_snapshot.http_bind_local,
        s_crash_snapshot.log_level, s_crash_snapshot.mirror_horizontal,
        s_crash_snapshot.mirror_vertical, s_crash_snapshot.preview_enabled,
        s_crash_snapshot.backlight_percent, s_crash_snapshot.screen_timeout_min);
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
    ipcam_sys_tune_cpu_policy();
    return 0;
}

void ipcam_sys_print_banner(void)
{
    fprintf(stderr,
        "=========================================================\n"
        "  ipcam  model=%s  swver=%s  git=%s  build=%s %s\n"
        "  param path: %s\n"
        "=========================================================\n",
        IPCAM_MODEL, IPCAM_VERSION, LIBIPCAM_GIT_INFO, __DATE__, __TIME__,
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
