#ifndef IPCAM_TOUCH_H
#define IPCAM_TOUCH_H

#include <pthread.h>
#include <signal.h>
#include <stdint.h>

#define IPCAM_TOUCH_MAX_POINTS 10

typedef struct ipcam_touch_point_s {
    int tracking_id;
    int x;
    int y;
    int active;
} ipcam_touch_point_t;

typedef void (*ipcam_touch_report_fn)(const ipcam_touch_point_t *points,
                                      int count, void *opaque);

typedef struct ipcam_touch_ctx_s {
    int fd;
    volatile sig_atomic_t service_running;
    pthread_t thread;
    ipcam_touch_report_fn report;
    void *opaque;
    ipcam_touch_point_t points[IPCAM_TOUCH_MAX_POINTS];
    int current_slot;
} ipcam_touch_ctx_t;

/* 读取 Linux evdev 多点协议并在 SYN_REPORT 时提交完整触点快照。 */
int ipcam_touch_start(ipcam_touch_ctx_t *ctx, const char *device,
                      ipcam_touch_report_fn report, void *opaque);
void ipcam_touch_stop(ipcam_touch_ctx_t *ctx);

#endif /* IPCAM_TOUCH_H */
