#define _GNU_SOURCE

/* evdev 触摸设备及触点状态日志归入 TOUCH 模块。 */
#define IPCAM_LOG_MODULE "TOUCH"
#include "ipcam_touch.h"
#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * 把驱动报告的 ABS 坐标线性映射到 LCD 像素坐标。
 * Goodix 在不同 BSP 上可能报告 0～799/0～479，也可能报告原始 ADC 范围；
 * 只有 EVIOCGABS 成功且范围有效时才映射，避免对已经是像素坐标的设备猜范围。
 */
static int map_axis(int value, int raw_min, int raw_max, int screen_size)
{
    if (screen_size <= 1 || raw_max <= raw_min) return value;
    if (value < raw_min) value = raw_min;
    if (value > raw_max) value = raw_max;
    return (int)(((long long)(value - raw_min) * (screen_size - 1)) /
                 (raw_max - raw_min));
}

/* SYN_REPORT 到达时复制活动触点，避免回调期间读到下一批 evdev 更新。 */
static void report_snapshot(ipcam_touch_ctx_t *ctx)
{
    ipcam_touch_point_t copy[IPCAM_TOUCH_MAX_POINTS];
    int count = 0;
    for (int i = 0; i < IPCAM_TOUCH_MAX_POINTS; i++) {
        if (!ctx->points[i].active) continue;
        copy[count] = ctx->points[i];
        copy[count].x = map_axis(copy[count].x, ctx->raw_min_x, ctx->raw_max_x,
                                 ctx->screen_w);
        copy[count].y = map_axis(copy[count].y, ctx->raw_min_y, ctx->raw_max_y,
                                 ctx->screen_h);
        count++;
    }
    if (count != ctx->last_report_count) {
        if (count > 0) {
            MLOGI("touch contacts changed: count=%d first=%d,%d id=%d\n",
                  count, copy[0].x, copy[0].y, copy[0].tracking_id);
        } else {
            MLOGI("touch contacts changed: count=0\n");
        }
        ctx->last_report_count = count;
    }
    if (ctx->report) ctx->report(copy, count, ctx->opaque);
}

/* 读取指定 ABS 轴范围；旧内核或非触摸设备不支持时保持未映射状态。 */
static void query_abs_range(ipcam_touch_ctx_t *ctx, unsigned int axis,
                            int *min_out, int *max_out)
{
    struct input_absinfo info;
    if (!ctx || !min_out || !max_out) return;
    memset(&info, 0, sizeof(info));
    if (ioctl(ctx->fd, EVIOCGABS(axis), &info) == 0 &&
        info.maximum > info.minimum) {
        *min_out = info.minimum;
        *max_out = info.maximum;
    }
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
                } else if (ev.code == ABS_X) {
                    /* 兼容没有 MT 轴、只上报单点 ABS_X/ABS_Y 的旧驱动。 */
                    ctx->points[0].x = ev.value;
                } else if (ev.code == ABS_Y) {
                    ctx->points[0].y = ev.value;
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
    return ipcam_touch_start_ex(ctx, device, 0, 0, report, opaque);
}

/*
 * 初始化 evdev 读取线程并记录显示尺寸。
 * 坐标范围通过 ioctl 在启动期读取一次，不在高频 SYN_REPORT 路径访问 sysfs；
 * 解析结果以像素快照回调给上层，既供双指手势使用，也供 LVGL pointer 使用。
 */
int ipcam_touch_start_ex(ipcam_touch_ctx_t *ctx, const char *device,
                         int screen_w, int screen_h,
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
    ctx->screen_w = screen_w;
    ctx->screen_h = screen_h;
    ctx->last_report_count = -1;
    query_abs_range(ctx, ABS_MT_POSITION_X, &ctx->raw_min_x, &ctx->raw_max_x);
    query_abs_range(ctx, ABS_MT_POSITION_Y, &ctx->raw_min_y, &ctx->raw_max_y);
    if (ctx->raw_max_x <= ctx->raw_min_x)
        query_abs_range(ctx, ABS_X, &ctx->raw_min_x, &ctx->raw_max_x);
    if (ctx->raw_max_y <= ctx->raw_min_y)
        query_abs_range(ctx, ABS_Y, &ctx->raw_min_y, &ctx->raw_max_y);
    /* 没有 evtest 时仍把 ioctl 探测结果写到串口，便于判断是节点错误、
     * 驱动范围异常还是后续 UI 坐标方向需要校正。 */
    MLOGI("touch device ready: %s screen=%dx%d raw_x=%d..%d raw_y=%d..%d\n",
          device, screen_w, screen_h, ctx->raw_min_x, ctx->raw_max_x,
          ctx->raw_min_y, ctx->raw_max_y);
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
    MLOGI("touch stop requested\n");
    ctx->service_running = 0;
    if (ctx->thread) { pthread_join(ctx->thread, NULL); ctx->thread = 0; }
    if (ctx->fd >= 0) { close(ctx->fd); ctx->fd = -1; }
    MLOGI("touch stopped\n");
}
