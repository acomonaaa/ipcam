#define _GNU_SOURCE

#include "ipcam_touch.h"
#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* SYN_REPORT 到达时复制活动触点，避免回调期间读到下一批 evdev 更新。 */
static void report_snapshot(ipcam_touch_ctx_t *ctx)
{
    ipcam_touch_point_t copy[IPCAM_TOUCH_MAX_POINTS];
    int count = 0;
    for (int i = 0; i < IPCAM_TOUCH_MAX_POINTS; i++) {
        if (ctx->points[i].active) copy[count++] = ctx->points[i];
    }
    if (ctx->report) ctx->report(copy, count, ctx->opaque);
}

/* 独立 evdev 线程只负责解析触点生命周期，不把按钮/缩放策略写死在驱动层。 */
static void *touch_thread(void *arg)
{
    ipcam_touch_ctx_t *ctx = arg;
    struct input_event ev;
    while (ctx->service_running) {
        ssize_t n = read(ctx->fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_ABS) {
                int slot = ctx->current_slot;
                if (slot < 0 || slot >= IPCAM_TOUCH_MAX_POINTS) slot = 0;
                if (ev.code == ABS_MT_SLOT) {
                    ctx->current_slot = ev.value;
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    ctx->points[slot].tracking_id = ev.value;
                    ctx->points[slot].active = ev.value >= 0;
                } else if (ev.code == ABS_MT_POSITION_X) {
                    ctx->points[slot].x = ev.value;
                } else if (ev.code == ABS_MT_POSITION_Y) {
                    ctx->points[slot].y = ev.value;
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0) {
                for (int i = 0; i < IPCAM_TOUCH_MAX_POINTS; i++) ctx->points[i].active = 0;
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                report_snapshot(ctx);
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (!ctx->service_running) break;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
            continue;
        }
        MLOGW("touch read failed: %s\n", n < 0 ? strerror(errno) : "short event");
        break;
    }
    return NULL;
}

/* 打开板级配置指定的输入节点；节点缺失时由 main 保持视频服务继续运行。 */
int ipcam_touch_start(ipcam_touch_ctx_t *ctx, const char *device,
                      ipcam_touch_report_fn report, void *opaque)
{
    if (!ctx || !device || !*device) return -1;
    memset(ctx, 0, sizeof(*ctx));
    /* 非阻塞读取使 stop 只需改变服务标志即可安全 join，不关闭可能被复用的 fd。 */
    ctx->fd = open(device, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (ctx->fd < 0) {
        MLOGW("open touch %s: %s\n", device, strerror(errno));
        return -1;
    }
    ctx->service_running = 1;
    ctx->current_slot = 0;
    ctx->report = report;
    ctx->opaque = opaque;
    for (int i = 0; i < IPCAM_TOUCH_MAX_POINTS; i++) ctx->points[i].tracking_id = -1;
    if (pthread_create(&ctx->thread, NULL, touch_thread, ctx) != 0) {
        close(ctx->fd); ctx->fd = -1; ctx->service_running = 0; return -1;
    }
    return 0;
}

/* 关闭 fd 唤醒 read，并等待线程退出，避免释放回调上下文后仍有异步访问。 */
void ipcam_touch_stop(ipcam_touch_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->service_running = 0;
    if (ctx->thread) { pthread_join(ctx->thread, NULL); ctx->thread = 0; }
    if (ctx->fd >= 0) { close(ctx->fd); ctx->fd = -1; }
}
