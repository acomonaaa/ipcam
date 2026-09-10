#ifndef IPCAM_SCREEN_H
#define IPCAM_SCREEN_H

/*
 * 本地显示电源策略：按单调时钟在超时前 10 秒显示休眠提示，超时后
 * 只暂停预览转换并关闭背光，不改变采集、编码、直播或录像生命周期；
 * 触摸唤醒后抑制首次输入，待全部触点释放再交给上层 GUI。
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
    int sleep_prompt_visible;
    int suppress_until_release;
    uint64_t last_touch_ns;
    uint64_t sleep_deadline_ns;
} ipcam_screen_ctx_t;

typedef struct ipcam_screen_status_s {
    int sleeping;
    int sleep_prompt_visible;
    int sleep_remaining_sec;
    int timeout_min;
} ipcam_screen_status_t;

/* 启动熄屏线程；timeout_min 允许 0/1/3/5/10，0 表示永不自动熄屏。 */
int ipcam_screen_start(ipcam_screen_ctx_t *ctx, volatile sig_atomic_t *running,
                       ipcam_display_ctx_t *display,
                       int brightness_percent, int timeout_min);
/* 提交当前活动触点数量，负责唤醒和首触摸抑制；提示期间不延长固定截止时间。 */
void ipcam_screen_touch(ipcam_screen_ctx_t *ctx, int active_points);
/* 更新亮度/超时运行时副本；参数已由控制器校验。 */
void ipcam_screen_update(ipcam_screen_ctx_t *ctx, int brightness_percent, int timeout_min);
/* 返回当前触摸是否可以继续传给 GUI。 */
int ipcam_screen_accept_input(ipcam_screen_ctx_t *ctx);
/* 读取休眠状态快照，供 LVGL 线程显示倒计时；不会阻塞 screen 计时线程。 */
int ipcam_screen_get_status(ipcam_screen_ctx_t *ctx,
                            ipcam_screen_status_t *status);
/* “立即休眠”按钮调用；状态切换后立即暂停本地预览并关闭背光。 */
int ipcam_screen_request_sleep(ipcam_screen_ctx_t *ctx);
/* “继续显示”按钮调用；隐藏提示并从当前时刻重新开始完整计时。 */
int ipcam_screen_keep_awake(ipcam_screen_ctx_t *ctx);
/* 停止线程并解除显示暂停，不修改共享进程退出标志。 */
void ipcam_screen_stop(ipcam_screen_ctx_t *ctx);

#endif /* IPCAM_SCREEN_H */
