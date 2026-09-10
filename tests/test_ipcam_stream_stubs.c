#include "ipcam_control.h"
#include "ipcam_display.h"
#include "ipcam_param.h"
#include "ipcam_record.h"
#include "ipcam_screen.h"

#include <string.h>

/*
 * 生命周期测试只覆盖 HTTP/ring 线程；这些桩隔离真实控制、LCD 和录像硬件，
 * 避免主机测试因为缺少 framebuffer、SD 卡或 V4L2 设备而改变测试目标。
 */
uint32_t ipcam_param_get_generation(void) { return 0; }
uint8_t ipcam_param_get_backlight_percent(void) { return 100; }
uint8_t ipcam_param_get_mirror_horizontal(void) { return 0; }
uint8_t ipcam_param_get_mirror_vertical(void) { return 0; }
uint8_t ipcam_param_get_preview_enabled(void) { return 1; }
uint8_t ipcam_param_get_screen_timeout_min(void) { return 0; }

int ipcam_param_set_mirror_horizontal(uint8_t value) { (void)value; return 0; }
int ipcam_param_set_mirror_vertical(uint8_t value) { (void)value; return 0; }
int ipcam_param_set_preview_enabled(uint8_t value) { (void)value; return 0; }
int ipcam_param_set_backlight_percent(uint8_t value) { (void)value; return 0; }
int ipcam_param_set_screen_timeout_min(uint8_t value) { (void)value; return 0; }

int ipcam_control_get_status(ipcam_control_ctx_t *ctx,
                             ipcam_control_status_t *out)
{
    (void)ctx;
    if (out) memset(out, 0, sizeof(*out));
    return -1;
}

int ipcam_control_get_capabilities(ipcam_control_ctx_t *ctx,
                                   ipcam_control_capabilities_t *out)
{
    (void)ctx;
    if (out) memset(out, 0, sizeof(*out));
    return -1;
}

int ipcam_control_submit_command(ipcam_control_ctx_t *ctx,
                                 const ipcam_control_command_t *command,
                                 uint64_t *request_id)
{
    (void)ctx;
    (void)command;
    if (request_id) *request_id = 0;
    return -1;
}

int ipcam_control_get_command_result(ipcam_control_ctx_t *ctx,
                                     uint64_t request_id,
                                     ipcam_control_result_t *out)
{
    (void)ctx;
    (void)request_id;
    if (out) memset(out, 0, sizeof(*out));
    return -1;
}

void ipcam_record_get_status(ipcam_record_ctx_t *ctx,
                             ipcam_record_status_t *out)
{
    (void)ctx;
    if (out) memset(out, 0, sizeof(*out));
}

/* 状态接口测试不启动真实录像线程；提供固定零快照，保持 HTTP 测试只关注
 * 请求生命周期和 ring 等待唤醒，不把存储服务链接进主机桩程序。 */
void ipcam_record_get_perf(ipcam_record_ctx_t *ctx, ipcam_record_perf_t *out)
{
    (void)ctx;
    if (out) memset(out, 0, sizeof(*out));
}

int ipcam_record_request_start(ipcam_record_ctx_t *ctx) { (void)ctx; return -1; }
int ipcam_record_request_stop(ipcam_record_ctx_t *ctx) { (void)ctx; return -1; }
int ipcam_record_save_photo(ipcam_record_ctx_t *ctx, char *path, size_t path_sz)
{
    (void)ctx;
    if (path && path_sz > 0) path[0] = '\0';
    return -1;
}

int ipcam_display_set_backlight_percent(int percent)
{
    (void)percent;
    return -1;
}

/* 流程测试不连接真实 framebuffer；补齐新的上下文背光接口，保持测试只验证 HTTP 生命周期。 */
int ipcam_display_backlight_available(ipcam_display_ctx_t *ctx)
{
    (void)ctx;
    return 0;
}

int ipcam_display_set_backlight(ipcam_display_ctx_t *ctx, int percent)
{
    (void)ctx;
    (void)percent;
    return -1;
}

void ipcam_display_get_view(ipcam_display_ctx_t *ctx, int *enabled,
                            float *zoom, float *center_x, float *center_y)
{
    (void)ctx;
    if (enabled) *enabled = 1;
    if (zoom) *zoom = 1.0f;
    if (center_x) *center_x = 0.5f;
    if (center_y) *center_y = 0.5f;
}

int ipcam_display_set_view(ipcam_display_ctx_t *ctx, int enabled,
                           float zoom, float center_x, float center_y)
{
    (void)ctx;
    (void)enabled;
    (void)zoom;
    (void)center_x;
    (void)center_y;
    return 0;
}

void ipcam_screen_update(ipcam_screen_ctx_t *ctx, int brightness_percent,
                         int timeout_min)
{
    (void)ctx;
    (void)brightness_percent;
    (void)timeout_min;
}
