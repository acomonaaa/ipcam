#ifndef IPCAM_SCREEN_H
#define IPCAM_SCREEN_H

/*
 * 本地显示电源策略：按单调时钟执行自动熄屏，熄屏只暂停预览转换并
 * 关闭背光，不改变采集、编码、直播或录像生命周期；触摸唤醒后抑制
 * 首次输入，待全部触点释放再交给上层 GUI。
 */

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include "ipcam_display.h"

typedef struct ipcam_screen_ctx_s {
    volatile sig_atomic_t *running;
    ipcam_display_ctx_t *display;
    volatile sig_atomic_t service_running;
    pthread_t thread;
    pthread_mutex_t mtx;
    int timeout_min;
    int brightness_percent;
    int sleeping;
    int suppress_until_release;
    uint64_t last_touch_ns;
} ipcam_screen_ctx_t;

/* 启动熄屏线程；timeout_min 允许 0/1/3/5/10，0 表示永不自动熄屏。 */
int ipcam_screen_start(ipcam_screen_ctx_t *ctx, volatile sig_atomic_t *running,
                       ipcam_display_ctx_t *display,
                       int brightness_percent, int timeout_min);
/* 提交当前活动触点数量，负责唤醒和首触摸抑制。 */
void ipcam_screen_touch(ipcam_screen_ctx_t *ctx, int active_points);
/* 更新亮度/超时运行时副本；参数已由控制器校验。 */
void ipcam_screen_update(ipcam_screen_ctx_t *ctx, int brightness_percent, int timeout_min);
/* 返回当前触摸是否可以继续传给 GUI。 */
int ipcam_screen_accept_input(ipcam_screen_ctx_t *ctx);
/* 停止线程并解除显示暂停，不修改共享进程退出标志。 */
void ipcam_screen_stop(ipcam_screen_ctx_t *ctx);

#endif /* IPCAM_SCREEN_H */
