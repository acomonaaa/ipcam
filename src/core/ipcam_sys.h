#ifndef IPCAM_SYS_H
#define IPCAM_SYS_H

/*
 * BCF2 mo_sys 风格的系统初始化 / 版本 / 横幅模块。
 *
 *  - ipcam_sys_init: 早于所有子系统调用；打印版本横幅 + 注册崩溃 handler
 *  - ipcam_sys_print_banner: 进程启动时打印
 *  - ipcam_sys_register_crash_handlers: 注册 SIGSEGV/SIGBUS/SIGILL handler dump param
 *  - ipcam_sys_init: 尝试将运行期 CPU governor 调到 performance，便于软件
 *    RGB565 转换和 LVGL 合成达到 15 FPS；sysfs 不存在时仅记录警告。
 */

#include "ipcam_log.h"

/* 库版本（与 BCF2 LIB_VERSION 风格一致：0xMMmmpp = Major.Minor.Patch） */
#define LIBIPCAM_LOG_VERSION       0x00010001
#define LIBIPCAM_PARAM_VERSION     0x00010001
#define LIBIPCAM_RINGBUF_VERSION   0x00010001
#define LIBIPCAM_CAPTURE_VERSION   0x00010001
#define LIBIPCAM_DISPLAY_VERSION   0x00010001
#define LIBIPCAM_ENCODE_VERSION    0x00010001
#define LIBIPCAM_STREAM_VERSION    0x00010001
#define LIBIPCAM_NET_VERSION       0x00010001

/*
 * 构建系统注入短 revision；未经过 Makefile 构建（例如单文件测试）时仍
 * 保留可识别的 unknown，而不是把固定分支名误报成实际运行二进制版本。
 */
#ifndef IPCAM_GIT_REVISION
#define IPCAM_GIT_REVISION   "unknown"
#endif
#define LIBIPCAM_GIT_INFO     IPCAM_GIT_REVISION
#define LIBIPCAM_BUILD_USER "build"

#define LIBIPCAM_LIB_VER_FMT(lib_name)                                   \
    do {                                                                 \
        unsigned char v1, v2, v3;                                        \
        v1 = (lib_name##_VERSION >> 16) & 0xff;                          \
        v2 = (lib_name##_VERSION >> 8)  & 0xff;                          \
        v3 = (lib_name##_VERSION)       & 0xff;                          \
        /* 启动版本信息归入 SYS 模块，保持 BCF2 按模块筛日志的习惯。 */    \
        MLOGI_M("SYS ", #lib_name " ver: %x.%x.%x_%s (build: %s %s)\n", \
              v1, v2, v3, LIBIPCAM_GIT_INFO, __DATE__, __TIME__);        \
    } while (0)

/* 早于所有子系统的初始化（注册崩溃 handler、log init） */
int  ipcam_sys_init(const char *module);

/* 进程启动横幅 */
void ipcam_sys_print_banner(void);

/* 打印所有子模块版本 */
void ipcam_sys_print_lib_versions(void);

/* 注册 SIGSEGV/SIGBUS/SIGILL handler（dump 缓存的 param 快照 + backtrace） */
void ipcam_sys_register_crash_handlers(void);

/* 在正常上下文调用，预拍一份 param 快照供 crash handler 异步安全读 */
void ipcam_sys_take_snapshot(void);

#endif /* IPCAM_SYS_H */
