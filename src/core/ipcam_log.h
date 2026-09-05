#ifndef IPCAM_LOG_H
#define IPCAM_LOG_H

/*
 * BCF2-style log module (adapted from mc-lib/libmo/libmolog/mo_log.h).
 *
 * 7 log levels:
 *   FATAL    - 必须立即处理（最高级）
 *   PRINT    - 直接打印（无时间戳、无前缀）
 *   ERROR    - 错误
 *   WARNING  - 警告
 *   INFO     - 普通信息
 *   DEBUG    - 调试（默认关闭，运行时可打开）
 *   BUTT     - 内部 sentinel
 *
 * 用法：
 *   ipcam_log_init("ipcam");            // 初始化（必须在 main 早期）
 *   ipcam_log_setlevel(IPCAM_LOG_INFO); // 运行时设置级别
 *   MLOGI("capture started w=%d h=%d\n", w, h);
 *
 * 与 BCF2 mo_log 的差异：
 *   - 函数前缀用 ipcam_ 而不是 mo_（避免命名冲突）
 *   - 模块名通过 ipcam_log_setmodule() 设定，默认 "ipcam"
 *   - 默认写 stderr；可用 ipcam_log_redirect_to_file() 切到文件
 */

#include <stdarg.h>
#include <stdint.h>

/* ANSI color codes (BCF2 风格) */
#define IPCAM_NONE          "\033[m"
#define IPCAM_NONE_N        "\033[m\n"
#define IPCAM_RED           "\033[0;32;31m"
#define IPCAM_LIGHT_RED     "\033[1;31m"
#define IPCAM_GREEN         "\033[0;32;32m"
#define IPCAM_LIGHT_GREEN   "\033[1;32m"
#define IPCAM_BLUE          "\033[0;32;34m"
#define IPCAM_LIGHT_BLUE    "\033[1;34m"
#define IPCAM_DARK_GREY     "\033[1;30m"
#define IPCAM_LIGHT_GREY    "\033[0;37m"
#define IPCAM_CYAN          "\033[0;36m"
#define IPCAM_LIGHT_CYAN    "\033[1;36m"
#define IPCAM_PURPLE        "\033[0;35m"
#define IPCAM_LIGHT_PURPLE  "\033[1;35m"
#define IPCAM_BROWN         "\033[0;33m"
#define IPCAM_YELLOW        "\033[1;33m"
#define IPCAM_WHITE         "\033[1;37m"

typedef enum {
    IPCAM_LOG_FATAL = 0,
    IPCAM_LOG_PRINT,
    IPCAM_LOG_ERROR,
    IPCAM_LOG_WARNING,
    IPCAM_LOG_INFO,
    IPCAM_LOG_DEBUG,
    IPCAM_LOG_BUTT
} ipcam_log_level_t;

#define IPCAM_LOG_MODULE_DEFAULT  "ipcam"

/* 公共 API（与 BCF2 mo_log_* 对齐） */
void ipcam_log_init(const char *module);
void ipcam_log_uninit(void);

/* 运行期设置日志级别；可被环境变量 IPCAM_LOG_LEVEL 覆盖（0..5） */
void ipcam_log_setlevel(ipcam_log_level_t level);
ipcam_log_level_t ipcam_log_getlevel(void);

/* 模块名前缀（多模块同进程可区分） */
void ipcam_log_setmodule(const char *module);

/* 线程号是否显示 */
void ipcam_log_enable_tid(int enable);

/* 重定向到文件（NULL = 关闭） */
int  ipcam_log_redirect_to_file(const char *path);

/* 核心 printf */
void ipcam_log_printf(ipcam_log_level_t level, const char *module,
                      const char *file, uint32_t line, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/*
 * 兼容旧 MLOG? 宏。这些宏调用 ipcam_log_printf：
 *   - module 传 NULL → 函数内部用 ipcam_log_setmodule() 设置的名字
 *   - file/line 透传 __FILE__/__LINE__
 *
 * 不再使用 IPCAM_LOG_MODULE_DEFAULT 硬编码 —— 否则 ipcam_log_setmodule
 * 就成了死代码。
 *
 * 注意：FORMAT 必须包含 \n。
 */
#define MLOGF(fmt, ...)  ipcam_log_printf(IPCAM_LOG_FATAL,   NULL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MLOGP(fmt, ...)  ipcam_log_printf(IPCAM_LOG_PRINT,   NULL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MLOGE(fmt, ...)  ipcam_log_printf(IPCAM_LOG_ERROR,   NULL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MLOGW(fmt, ...)  ipcam_log_printf(IPCAM_LOG_WARNING, NULL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MLOGI(fmt, ...)  ipcam_log_printf(IPCAM_LOG_INFO,    NULL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MLOGD(fmt, ...)  ipcam_log_printf(IPCAM_LOG_DEBUG,   NULL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* IPCAM_LOG_H */