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
static int               s_out_fd    = -1;     /* dup 出来的 fd */
static int               s_out_bak   = -1;    /* 原始 stderr fd 备份 */

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

void ipcam_log_setlevel(ipcam_log_level_t level)
{
    if (level >= IPCAM_LOG_BUTT) level = IPCAM_LOG_BUTT - 1;
    s_level = level;
}

ipcam_log_level_t ipcam_log_getlevel(void)
{
    return s_level;
}

void ipcam_log_setmodule(const char *module)
{
    if (!module || !*module) return;
    strncpy(s_module, module, sizeof(s_module) - 1);
    s_module[sizeof(s_module) - 1] = '\0';
}

void ipcam_log_enable_tid(int enable)
{
    s_enable_tid = enable ? 1 : 0;
}

static void detect_color(void)
{
    /* stderr 重定向到文件时关掉颜色 */
    s_use_color = s_out == NULL && isatty(STDERR_FILENO);
}

void ipcam_log_init(const char *module)
{
    if (module && *module) ipcam_log_setmodule(module);

    s_out = NULL;
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

    /* 简单的级别公告，方便看进程启动 */
    fprintf(stderr, "%s[ipcam] log init: module=%s level=%d(%s) color=%d\n",
            s_use_color ? IPCAM_DARK_GREY : "",
            s_module, (int)s_level, s_level_name[(int)s_level], s_use_color);
}

void ipcam_log_uninit(void)
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

int ipcam_log_redirect_to_file(const char *path)
{
    if (!path) {
        ipcam_log_uninit();
        return 0;
    }

    /* 先还原旧的，避免泄漏 fd */
    ipcam_log_uninit();

    FILE *fp = fopen(path, "a");
    if (!fp) return -1;
    setvbuf(fp, NULL, _IOLBF, 0);  /* 行缓冲，及时落盘 */

    s_out_bak = dup(STDERR_FILENO);
    s_out_fd  = dup(fileno(fp));
    if (s_out_fd >= 0 && dup2(s_out_fd, STDERR_FILENO) >= 0) {
        s_out = fp;
        detect_color();
        return 0;
    }

    fclose(fp);
    return -1;
}

void ipcam_log_printf(ipcam_log_level_t level, const char *module,
                      const char *file, uint32_t line, const char *fmt, ...)
{
    if (level >= IPCAM_LOG_BUTT) return;
    if (level == IPCAM_LOG_PRINT) {
        /* PRINT 级别直打，无前缀 */
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fflush(stderr);
        return;
    }

    if (level > s_level) return;  /* 低于阈值的不打印 */

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
}