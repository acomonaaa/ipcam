#define _GNU_SOURCE
#include "ipcam_log.h"
#include "ipcam_config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ===== 状态 ===== */
static ipcam_log_level_t s_level  = IPCAM_LOG_INFO;
static char              s_module[64] = IPCAM_LOG_MODULE_DEFAULT;
static int               s_enable_tid = 1;
static int               s_use_color  = 1;     /* stderr 是 tty 时才上色 */
static FILE             *s_out       = NULL;   /* NULL = stderr */
static char              s_out_path[256];
static int               s_out_fd    = -1;     /* dup 出来的 fd */
static int               s_out_bak   = -1;    /* 原始 stderr fd 备份 */
/* BCF2 风格日志必须以“整条记录”为粒度输出，否则多线程前缀与正文会交错。 */
static pthread_mutex_t   s_print_mtx = PTHREAD_MUTEX_INITIALIZER;

#define IPCAM_LOG_ROTATE_BYTES (5U * 1024U * 1024U)
#define IPCAM_LOG_ROTATE_KEEP  3

/* BCF2 风格：每个 level 一个颜色 */
static const char *s_level_color[IPCAM_LOG_BUTT + 1] = {
    IPCAM_LIGHT_PURPLE,  /* FATAL */
    IPCAM_NONE,          /* PRINT */
    IPCAM_LIGHT_RED,     /* ERROR */
    IPCAM_YELLOW,        /* WARNING */
    IPCAM_LIGHT_GREEN,   /* INFO */
    IPCAM_LIGHT_BLUE,    /* DEBUG */
    IPCAM_WHITE          /* BUTT */
};

static const char *s_level_name[IPCAM_LOG_BUTT + 1] = {
    "FAT", "PRI", "ERR", "WRN", "INF", "DBG", "???"
};

static void strip_path(const char *in, char *out, size_t out_sz)
{
    const char *p = strrchr(in, '/');
    p = p ? p + 1 : in;
    strncpy(out, p, out_sz - 1);
    out[out_sz - 1] = '\0';
}

/* 运行时切换等级；与整条日志共用同一把锁，避免阈值改变时读到半状态。 */
void ipcam_log_setlevel(ipcam_log_level_t level)
{
    if (level >= IPCAM_LOG_BUTT) level = IPCAM_LOG_BUTT - 1;
    pthread_mutex_lock(&s_print_mtx);
    s_level = level;
    pthread_mutex_unlock(&s_print_mtx);
}

/* 读取当前等级；返回受锁保护的快照，调用方无需持有日志锁。 */
ipcam_log_level_t ipcam_log_getlevel(void)
{
    pthread_mutex_lock(&s_print_mtx);
    ipcam_log_level_t level = s_level;
    pthread_mutex_unlock(&s_print_mtx);
    return level;
}

/* 更新模块名；日志格式化时复制/读取均受同一把输出锁保护。 */
void ipcam_log_setmodule(const char *module)
{
    if (!module || !*module) return;
    pthread_mutex_lock(&s_print_mtx);
    strncpy(s_module, module, sizeof(s_module) - 1);
    s_module[sizeof(s_module) - 1] = '\0';
    pthread_mutex_unlock(&s_print_mtx);
}

/* 开关线程号字段；避免与正在输出的记录交错。 */
void ipcam_log_enable_tid(int enable)
{
    pthread_mutex_lock(&s_print_mtx);
    s_enable_tid = enable ? 1 : 0;
    pthread_mutex_unlock(&s_print_mtx);
}

static void detect_color(void)
{
    /* stderr 重定向到文件时关掉颜色 */
    s_use_color = s_out == NULL && isatty(STDERR_FILENO);
}

/* 初始化日志状态；启动脚本可用 IPCAM_LOG_FILE 把 stderr 接到串口控制台。 */
void ipcam_log_init(const char *module)
{
    if (module && *module) ipcam_log_setmodule(module);

    s_out = NULL;
    s_out_path[0] = '\0';
    s_out_fd = -1;
    s_out_bak = -1;
    detect_color();

    /* 默认级别来自编译期 IPCAM_LOG_LEVEL（ipcam_config.h），运行期可被 IPCAM_LOG_LEVEL 环境变量覆盖 */
    s_level = (ipcam_log_level_t)IPCAM_LOG_LEVEL;
    if (IPCAM_LOG_LEVEL < 0 || IPCAM_LOG_LEVEL >= IPCAM_LOG_BUTT) s_level = IPCAM_LOG_INFO;

    const char *e = getenv("IPCAM_LOG_LEVEL");
    if (e && *e) {
        int v = atoi(e);
        if (v >= 0 && v < IPCAM_LOG_BUTT) s_level = (ipcam_log_level_t)v;
    }

    /* 默认写入 tmpfs，避免高频日志持续写入板载闪存；失败时保留 stderr。 */
    const char *log_path = getenv("IPCAM_LOG_FILE");
    if (!log_path || !*log_path) log_path = "/tmp/ipcam/ipcam.log";
    char log_dir[256];
    snprintf(log_dir, sizeof(log_dir), "%s", log_path);
    char *slash = strrchr(log_dir, '/');
    if (slash) { *slash = '\0'; if (*log_dir) mkdir(log_dir, 0755); }
    ipcam_log_redirect_to_file(log_path);

    /* 公告实际生效的位置；/dev/console 失败时明确显示已经回退到 stderr。 */
    const char *effective_path = s_out_path[0] ? s_out_path : "stderr";
    fprintf(stderr, "%s[ipcam] log init: module=%s level=%d(%s) color=%d file=%s\n",
            s_use_color ? IPCAM_DARK_GREY : "",
            s_module, (int)s_level, s_level_name[(int)s_level], s_use_color,
            effective_path);
}

/* 仅在 s_print_mtx 已持有时调用，避免日志轮转再次获取同一把锁。 */
static void log_uninit_unlocked(void)
{
    if (s_out && s_out != stderr) {
        fflush(s_out);
        fclose(s_out);
        s_out = NULL;
    }
    if (s_out_fd >= 0) {
        if (s_out_bak >= 0) {
            dup2(s_out_bak, STDERR_FILENO);
            close(s_out_bak);
            s_out_bak = -1;
        }
        close(s_out_fd);
        s_out_fd = -1;
    }
}

/* 在所有日志调用结束后关闭文件并恢复 stderr；调用者不应在并发退出中重复销毁。 */
void ipcam_log_uninit(void)
{
    pthread_mutex_lock(&s_print_mtx);
    log_uninit_unlocked();
    pthread_mutex_unlock(&s_print_mtx);
}

/* 仅在 s_print_mtx 已持有时切换输出；轮转与显式重定向共用此实现。 */
static int log_redirect_to_file_unlocked(const char *path)
{
    if (!path) {
        log_uninit_unlocked();
        s_out_path[0] = '\0';
        return 0;
    }

    /* 先还原旧的，避免泄漏 fd */
    log_uninit_unlocked();

    struct stat st;
    if (stat(path, &st) == 0 && st.st_size >= IPCAM_LOG_ROTATE_BYTES) {
        char old[320], next[320];
        for (int i = IPCAM_LOG_ROTATE_KEEP - 1; i >= 1; i--) {
            snprintf(old, sizeof(old), "%s.%d", path, i);
            snprintf(next, sizeof(next), "%s.%d", path, i + 1);
            if (i == IPCAM_LOG_ROTATE_KEEP - 1) unlink(next);
            rename(old, next);
        }
        snprintf(next, sizeof(next), "%s.1", path);
        rename(path, next);
    }
    FILE *fp = fopen(path, "a");
    if (!fp) return -1;
    setvbuf(fp, NULL, _IOLBF, 0);  /* 行缓冲，及时落盘 */

    s_out_bak = dup(STDERR_FILENO);
    s_out_fd  = dup(fileno(fp));
    if (s_out_bak >= 0 && s_out_fd >= 0 && dup2(s_out_fd, STDERR_FILENO) >= 0) {
        s_out = fp;
        snprintf(s_out_path, sizeof(s_out_path), "%s", path);
        detect_color();
        return 0;
    }

    if (s_out_fd >= 0) { close(s_out_fd); s_out_fd = -1; }
    if (s_out_bak >= 0) { close(s_out_bak); s_out_bak = -1; }
    fclose(fp);
    return -1;
}

/* 运行时切换日志文件；轮转、fd 备份和颜色开关在同一把锁内完成。 */
int ipcam_log_redirect_to_file(const char *path)
{
    pthread_mutex_lock(&s_print_mtx);
    int rc = log_redirect_to_file_unlocked(path);
    pthread_mutex_unlock(&s_print_mtx);
    return rc;
}

/* 先完整格式化一条记录再统一写出，避免多线程前缀、正文和轮转互相穿插。 */
void ipcam_log_printf(ipcam_log_level_t level, const char *module,
                      const char *file, uint32_t line, const char *fmt, ...)
{
    if (level >= IPCAM_LOG_BUTT) return;
    pthread_mutex_lock(&s_print_mtx);
    if (level == IPCAM_LOG_PRINT) {
        /* PRINT 级别直打，无前缀 */
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fflush(stderr);
        pthread_mutex_unlock(&s_print_mtx);
        return;
    }

    if (level > s_level) {
        pthread_mutex_unlock(&s_print_mtx);
        return;  /* 低于阈值的不打印 */
    }

    /* 时间戳（HH:MM:SS.mmm） */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    const char *use_module = (module && *module) ? module : s_module;
    const char *use_file   = file ? file : "?";
    const char *color = s_use_color ? s_level_color[level] : "";
    const char *reset = s_use_color ? IPCAM_NONE : "";

    /* 取 basename，避免整路径 */
    char fb[64];
    strip_path(use_file, fb, sizeof(fb));

    if (s_enable_tid) {
        unsigned long tid = (unsigned long)pthread_self();
        fprintf(stderr, "%s%s.%03ld %s [%s] %s:%u (tid=%lx) %s",
                color, ts, (long)(tv.tv_usec / 1000),
                s_level_name[level], use_module,
                fb, line, tid, reset);
    } else {
        fprintf(stderr, "%s%s.%03ld %s [%s] %s:%u %s",
                color, ts, (long)(tv.tv_usec / 1000),
                s_level_name[level], use_module,
                fb, line, reset);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    /* 用户 fmt 没带 \n 时补一个，避免刷屏串行 */
    size_t fl = strlen(fmt);
    if (fl == 0 || fmt[fl - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
    if (s_out && s_out_path[0] && ftell(s_out) >= (long)IPCAM_LOG_ROTATE_BYTES) {
        char path_copy[256];
        snprintf(path_copy, sizeof(path_copy), "%s", s_out_path);
        log_redirect_to_file_unlocked(path_copy);
    }
    pthread_mutex_unlock(&s_print_mtx);
}
