#define _GNU_SOURCE

/* 统一控制命令的受理、校验和结果日志归入 CTRL 模块。 */
#define IPCAM_LOG_MODULE "CTRL"
#include "ipcam_control.h"
#include "ipcam_config.h"
#include "ipcam_log.h"
#include "ipcam_param.h"
#include "ipcam_storage.h"
#include "ipcam_touch.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <time.h>

/*
 * 首版只把 640×480@15 作为对外能力。驱动即使声称支持更多档位，
 * 在完成板端压力测试前也不能让控制入口写入无法实时保证的组合。
 */
/* 探测真正的 Type-B 多点输入轴；仅“节点存在”不能证明双指能力。 */
static uint8_t probe_touch_points(void)
{
    const char *touch_dev = getenv("IPCAM_TOUCH_DEV");
    if (!touch_dev || !*touch_dev) return 0;
    int fd = open(touch_dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct input_absinfo slot, x, y;
    int ok = ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0 &&
             ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x) == 0 &&
             ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y) == 0;
    close(fd);
    if (!ok || slot.maximum < slot.minimum) return 0;
    int points = slot.maximum - slot.minimum + 1;
    if (points < 2) return 0;
    if (points > IPCAM_TOUCH_MAX_POINTS) points = IPCAM_TOUCH_MAX_POINTS;
    return (uint8_t)points;
}

/* 将统一控制命令转换成稳定的日志文本；不把无关的请求体原样打到串口。 */
static const char *control_command_name(ipcam_control_command_type_t type)
{
    switch (type) {
    case IPCAM_CONTROL_SET_VIDEO: return "set_video";
    case IPCAM_CONTROL_SET_MIRROR: return "set_mirror";
    case IPCAM_CONTROL_SET_PREVIEW: return "set_preview";
    case IPCAM_CONTROL_SET_VIEW: return "set_view";
    case IPCAM_CONTROL_SET_BACKLIGHT: return "set_backlight";
    case IPCAM_CONTROL_SET_LIGHT: return "set_light";
    case IPCAM_CONTROL_SET_SCREEN_TIMEOUT: return "set_screen_timeout";
    case IPCAM_CONTROL_RECORD_START: return "record_start";
    case IPCAM_CONTROL_RECORD_STOP: return "record_stop";
    case IPCAM_CONTROL_PHOTO: return "photo";
    case IPCAM_CONTROL_FORMAT_STORAGE: return "format_storage";
    default: return "unknown";
    }
}

/* 只打印会影响设备行为的参数，便于复现 GUI/HTTP/camctl 发出的控制请求。 */
static void control_log_command(const ipcam_control_command_t *cmd,
                                uint64_t request_id)
{
    if (!cmd) return;
    switch (cmd->type) {
    case IPCAM_CONTROL_SET_VIDEO:
        MLOGI("command submit id=%llu name=%s video=%ux%u@%u q=%u\n",
              (unsigned long long)request_id, control_command_name(cmd->type),
              cmd->video.width, cmd->video.height, cmd->video.target_fps,
              cmd->video.jpeg_quality);
        break;
    case IPCAM_CONTROL_SET_MIRROR:
        MLOGI("command submit id=%llu name=%s mirror_h=%d mirror_v=%d\n",
              (unsigned long long)request_id, control_command_name(cmd->type),
              cmd->mirror_horizontal, cmd->mirror_vertical);
        break;
    case IPCAM_CONTROL_SET_PREVIEW:
        MLOGI("command submit id=%llu name=%s enabled=%d\n",
              (unsigned long long)request_id, control_command_name(cmd->type),
              cmd->enabled);
        break;
    case IPCAM_CONTROL_SET_VIEW:
        MLOGI("command submit id=%llu name=%s enabled=%d zoom=%.2f center=%.3f,%.3f\n",
              (unsigned long long)request_id, control_command_name(cmd->type),
              cmd->enabled, cmd->zoom, cmd->center_x, cmd->center_y);
        break;
    case IPCAM_CONTROL_SET_BACKLIGHT:
    case IPCAM_CONTROL_SET_LIGHT:
        MLOGI("command submit id=%llu name=%s percent=%d\n",
              (unsigned long long)request_id, control_command_name(cmd->type),
              cmd->percent);
        break;
    case IPCAM_CONTROL_SET_SCREEN_TIMEOUT:
        MLOGI("command submit id=%llu name=%s timeout_min=%d\n",
              (unsigned long long)request_id, control_command_name(cmd->type),
              cmd->timeout_min);
        break;
    default:
        MLOGI("command submit id=%llu name=%s\n",
              (unsigned long long)request_id, control_command_name(cmd->type));
        break;
    }
}

static void fill_capabilities(const ipcam_control_ctx_t *ctx,
                              ipcam_control_capabilities_t *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->video[0].width = IPCAM_CAPTURE_WIDTH;
    caps->video[0].height = IPCAM_CAPTURE_HEIGHT;
    caps->video[0].target_fps = IPCAM_TARGET_FPS;
    caps->video[0].jpeg_quality = IPCAM_JPEG_QUALITY;
    caps->video_count = 1;
    caps->jpeg_quality_min = 1;
    caps->jpeg_quality_max = 100;
    caps->adaptive_quality = 1;
    const char *adaptive_env = getenv("IPCAM_JPEG_ADAPTIVE");
    if (adaptive_env && !strcmp(adaptive_env, "0")) caps->adaptive_quality = 0;
    caps->jpeg_quality_levels[0] = IPCAM_JPEG_QUALITY;
    caps->jpeg_quality_levels[1] = IPCAM_JPEG_QUALITY > 5 ? IPCAM_JPEG_QUALITY - 5 : 1;
    caps->jpeg_quality_levels[2] = IPCAM_JPEG_QUALITY > 10 ? IPCAM_JPEG_QUALITY - 10 : 1;
    caps->mirror_horizontal = 1;
    caps->mirror_vertical = 1;
    /* 没有成功打开 LCD 时，本地视口和熄屏服务都不可用；网络/录像
     * 仍可工作，但能力接口不能把显示功能报告成已验证。 */
    caps->preview_zoom = ctx && ctx->display ? 1 : 0;
    caps->preview_zoom_min = 1.0f;
    caps->preview_zoom_max = 4.0f;
    caps->touch_points = probe_touch_points();
    /* display 已完成 env/sysfs 探测，能力值必须复用实际探测结果，不能只看
     * IPCAM_BACKLIGHT_PATH；否则自动发现的 pwm-backlight 会被错误报告为不可用。 */
    caps->backlight = ctx && ctx->display &&
                      ipcam_display_backlight_available(ctx->display);
    caps->light = ipcam_light_available();
    caps->screen_timeout = ctx && ctx->display ? 1 : 0;
    caps->record_segment_seconds = IPCAM_RECORD_SEGMENT_SECONDS;
}

/* 采集协商成功后，以驱动实际值替换编译期默认，避免能力接口报错尺寸。 */
static void overlay_capture_capability(const ipcam_control_ctx_t *ctx,
                                       ipcam_control_capabilities_t *caps)
{
    if (!ctx || !ctx->capture || ctx->capture->width <= 0 || ctx->capture->height <= 0)
        return;
    caps->video[0].width = (uint16_t)ctx->capture->width;
    caps->video[0].height = (uint16_t)ctx->capture->height;
    if (ctx->capture->target_fps > 0)
        caps->video[0].target_fps = (uint8_t)ctx->capture->target_fps;
}

/* 在结果环中按 request_id 查找槽位；调用者必须持有 ctx->mtx。 */
static ipcam_control_result_t *find_result_locked(ipcam_control_ctx_t *ctx,
                                                   uint64_t request_id)
{
    for (size_t i = 0; i < sizeof(ctx->results) / sizeof(ctx->results[0]); i++) {
        if (ctx->results[i].request_id == request_id) return &ctx->results[i];
    }
    return NULL;
}

/* 读取网络状态；调用频率由 control_refresh_status 限制到最多每 2 秒一次。 */
static void fill_network_status(ipcam_control_status_t *status)
{
    status->network_link = 0;
    status->network_ip[0] = '\0';
    const char *iface = getenv("IPCAM_NET_IFACE");
    if (!iface || !*iface) iface = "eth0";
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs *it = list; it; it = it->ifa_next) {
            if (!it->ifa_addr || strcmp(it->ifa_name, iface) != 0 ||
                it->ifa_addr->sa_family != AF_INET) continue;
            struct sockaddr_in *sin = (struct sockaddr_in *)it->ifa_addr;
            if (inet_ntop(AF_INET, &sin->sin_addr, status->network_ip,
                          sizeof(status->network_ip)))
                status->network_link = 1;
            break;
        }
        freeifaddrs(list);
    }
}

/* 读取 SD 身份/空间；该函数只在低频状态刷新阶段执行，不允许放进 get_status。 */
static void fill_storage_status(ipcam_control_status_t *status,
                                 ipcam_record_ctx_t *recorder)
{
    /* 每次低频刷新先清掉上一轮身份；未挂载时不能残留旧设备名。 */
    status->storage_mounted = 0;
    status->storage_total_bytes = 0;
    status->storage_used_bytes = 0;
    status->storage_available_bytes = 0;
    status->storage_format_supported = 0;
    memset(status->storage_mount_path, 0, sizeof(status->storage_mount_path));
    memset(status->storage_device, 0, sizeof(status->storage_device));
    memset(status->storage_fs_type, 0, sizeof(status->storage_fs_type));
    if (recorder) {
        ipcam_storage_info_t info;
        memset(&info, 0, sizeof(info));
        (void)ipcam_storage_get_info(recorder->storage_root, &info);
        status->storage_mounted = info.mounted ? 1 : 0;
        status->storage_total_bytes = info.total_bytes;
        status->storage_used_bytes = info.used_bytes;
        status->storage_available_bytes = info.available_bytes;
        status->storage_format_supported = info.format_supported ? 1 : 0;
        snprintf(status->storage_mount_path, sizeof(status->storage_mount_path), "%s",
                 info.mount_path);
        snprintf(status->storage_device, sizeof(status->storage_device), "%s",
                 info.device);
        snprintf(status->storage_fs_type, sizeof(status->storage_fs_type), "%s",
                 info.fs_type);
    }
}

/* 状态刷新本身不能使用墙上时钟；单调时钟只用于控制探测节流。 */
static uint64_t control_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * 填写不会触发文件系统/网络探测的快速状态部分。调用者传入的 status
 * 可能带有上一轮低频网络和存储快照，故本函数只覆盖实时服务字段，避免
 * HTTP 查询因为 getifaddrs/statvfs 进入媒体线程的慢路径。
 */
static void fill_fast_status(const ipcam_control_ctx_t *ctx,
                             ipcam_control_status_t *status)
{
    if (!ctx || !status) return;
    status->video.width = ctx->capture && ctx->capture->width > 0 ?
                          (uint16_t)ctx->capture->width : ipcam_param_get_capture_w();
    status->video.height = ctx->capture && ctx->capture->height > 0 ?
                           (uint16_t)ctx->capture->height : ipcam_param_get_capture_h();
    status->video.target_fps = ipcam_param_get_target_fps();
    status->video.jpeg_quality = ipcam_param_get_jpeg_quality();
    status->configured_jpeg_quality = status->video.jpeg_quality;
    status->mirror_horizontal = ipcam_param_get_mirror_horizontal();
    status->mirror_vertical = ipcam_param_get_mirror_vertical();
    status->preview_enabled = ipcam_param_get_preview_enabled();
    status->backlight_percent = ipcam_param_get_backlight_percent();
    status->light_percent = (uint8_t)ipcam_light_get_percent();
    status->screen_timeout_min = ipcam_param_get_screen_timeout_min();
    status->config_generation = ipcam_param_get_generation();
    status->capture_fps = 0;
    status->output_fps = 0;
    status->preview_view_enabled = 0;
    status->preview_zoom = 1.0f;
    status->preview_center_x = 0.5f;
    status->preview_center_y = 0.5f;
    status->jpeg_live_frames = 0;
    status->jpeg_live_dropped = 0;
    status->jpeg_record_frames = 0;
    status->jpeg_record_dropped = 0;
    status->capture_frames = 0;
    status->capture_dropped_display = 0;
    status->capture_dropped_encode = 0;
    memset(&status->encode_perf, 0, sizeof(status->encode_perf));
    memset(&status->display_perf, 0, sizeof(status->display_perf));
    memset(&status->record_perf, 0, sizeof(status->record_perf));
    memset(&status->record, 0, sizeof(status->record));
    snprintf(status->pipeline, sizeof(status->pipeline), "%s",
             "capture->yuyv420->jpeg->live+record");
    snprintf(status->framebuffer_mode, sizeof(status->framebuffer_mode), "%s",
             "unavailable");

    if (ctx->capture) {
        status->capture_fps = ctx->capture->actual_fps;
        if (ctx->capture->actual_fps == 0 ||
            ctx->capture->actual_fps >= ctx->capture->target_fps)
            status->output_fps = ctx->capture->target_fps;
        else
            status->output_fps = ctx->capture->actual_fps;
        ipcam_capture_get_stats((ipcam_capture_ctx_t *)ctx->capture,
                                &status->capture_frames,
                                &status->capture_dropped_display,
                                &status->capture_dropped_encode);
    }
    if (ctx->display) {
        ipcam_display_get_view(ctx->display, &status->preview_view_enabled,
                               &status->preview_zoom, &status->preview_center_x,
                               &status->preview_center_y);
        ipcam_display_get_perf(ctx->display, &status->display_perf);
        snprintf(status->framebuffer_mode, sizeof(status->framebuffer_mode), "%s",
                 status->display_perf.framebuffer_mode[0] ?
                 status->display_perf.framebuffer_mode : "unknown");
    }
    if (ctx->jpeg_live_rb) {
        status->jpeg_live_frames = ipcam_ring_count(ctx->jpeg_live_rb);
        status->jpeg_live_dropped = ipcam_ring_dropped_count(ctx->jpeg_live_rb);
    }
    if (ctx->recorder) {
        ipcam_record_get_status(ctx->recorder, &status->record);
        ipcam_record_get_perf(ctx->recorder, &status->record_perf);
        status->jpeg_record_frames = ipcam_ring_count(ctx->recorder->jpeg_rb);
        status->jpeg_record_dropped = ipcam_ring_dropped_count(ctx->recorder->jpeg_rb);
    }
    if (ctx->encoder) {
        ipcam_encode_get_perf(ctx->encoder, &status->encode_perf);
        status->configured_jpeg_quality = status->encode_perf.configured_quality;
        status->effective_jpeg_quality = status->encode_perf.effective_quality;
        status->adaptive_quality = status->encode_perf.adaptive_quality;
    } else {
        status->effective_jpeg_quality = status->configured_jpeg_quality;
        const char *adaptive_env = getenv("IPCAM_JPEG_ADAPTIVE");
        status->adaptive_quality = !(adaptive_env && !strcmp(adaptive_env, "0"));
    }
}

/* 统一填写成功/失败状态，避免不同命令入口返回含义不一致。 */
static void result_message(ipcam_control_result_t *result, int rc,
                           int persisted, const char *message)
{
    result->state = rc == 0 ? IPCAM_CONTROL_RESULT_DONE : IPCAM_CONTROL_RESULT_FAILED;
    result->error_code = rc == 0 ? 0 : rc;
    result->persisted = persisted;
    snprintf(result->message, sizeof(result->message), "%s", message ? message :
             (rc == 0 ? "命令完成" : "命令失败"));
}

/* 录像期间分辨率、帧率和翻转必须保持稳定；JPEG 质量/补光仍可调整。 */
static int recording_locks_video(ipcam_control_ctx_t *ctx)
{
    if (!ctx || !ctx->recorder) return 0;
    ipcam_record_status_t status;
    ipcam_record_get_status(ctx->recorder, &status);
    return status.state == IPCAM_RECORD_STARTING ||
           status.state == IPCAM_RECORD_RECORDING ||
           status.state == IPCAM_RECORD_STOPPING;
}

/* 在控制器锁内执行一个已分配 request_id 的命令。 */
static int apply_command(ipcam_control_ctx_t *ctx,
                          const ipcam_control_command_t *cmd,
                          ipcam_control_result_t *result)
{
    ipcam_control_capabilities_t caps;
    fill_capabilities(ctx, &caps);
    overlay_capture_capability(ctx, &caps);
    result->persisted = 0;

    switch (cmd->type) {
    case IPCAM_CONTROL_SET_VIDEO:
        if (recording_locks_video(ctx) &&
            (cmd->video.width != ipcam_param_get_capture_w() ||
             cmd->video.height != ipcam_param_get_capture_h() ||
             cmd->video.target_fps != ipcam_param_get_target_fps())) {
            result_message(result, -14, 0, "录像期间分辨率和帧率已锁定");
            return -14;
        }
        /* 当前媒体线程没有安全的热切换窗口，未验证档位明确拒绝。 */
        if (cmd->video.width != caps.video[0].width ||
            cmd->video.height != caps.video[0].height ||
            cmd->video.target_fps != caps.video[0].target_fps) {
            result_message(result, -2, 0, "视频档位未验证，当前仅支持 640x480@15fps");
            return -2;
        }
        if (cmd->video.jpeg_quality < caps.jpeg_quality_min ||
            cmd->video.jpeg_quality > caps.jpeg_quality_max ||
            ipcam_param_set_video_group(cmd->video.width, cmd->video.height,
                                         cmd->video.target_fps,
                                         cmd->video.jpeg_quality) != 0) {
            result_message(result, -3, 0, "JPEG 质量不合法或保存失败");
            return -3;
        }
        result_message(result, 0, 1, "视频参数已生效并保存");
        return 0;

    case IPCAM_CONTROL_SET_MIRROR:
        if (recording_locks_video(ctx)) {
            result_message(result, -15, 0, "录像期间翻转设置已锁定");
            return -15;
        }
        if ((cmd->mirror_horizontal != 0 && cmd->mirror_horizontal != 1) ||
            (cmd->mirror_vertical != 0 && cmd->mirror_vertical != 1) ||
            ipcam_param_set_mirror_pair((uint8_t)cmd->mirror_horizontal,
                                         (uint8_t)cmd->mirror_vertical) != 0) {
            result_message(result, -4, 0, "翻转参数不合法或保存失败");
            return -4;
        }
        result_message(result, 0, 1, "翻转参数已生效并保存");
        return 0;

    case IPCAM_CONTROL_SET_PREVIEW: {
        if (cmd->enabled != 0 && cmd->enabled != 1) {
            result_message(result, -5, 0, "预览开关不合法");
            return -5;
        }
        int old_enabled = 1;
        float zoom = 1.0f, center_x = 0.5f, center_y = 0.5f;
        if (ctx->display)
            ipcam_display_get_view(ctx->display, &old_enabled, &zoom, &center_x, &center_y);
        /* 先应用运行时视图，再落盘；若保存失败恢复旧视图，避免“已生效/已保存”
         * 两个状态分裂。没有 LCD 时预览命令也必须明确失败。 */
        if (!ctx->display || ipcam_display_set_view(ctx->display, cmd->enabled,
                                                     zoom, center_x, center_y) != 0) {
            result_message(result, -6, 0, "预览开关应用失败");
            return -6;
        }
        if (ipcam_param_set_preview_enabled((uint8_t)cmd->enabled) != 0) {
            ipcam_display_set_view(ctx->display, old_enabled, zoom, center_x, center_y);
            result_message(result, -6, 0, "预览开关保存失败，已回退");
            return -6;
        }
        result_message(result, 0, 1, "预览开关已生效并保存");
        return 0;
    }

    case IPCAM_CONTROL_SET_VIEW:
        if (!ctx->display || ipcam_display_set_view(ctx->display, cmd->enabled,
                                                     cmd->zoom, cmd->center_x,
                                                     cmd->center_y) != 0) {
            result_message(result, -7, 0, "预览视口不合法或显示服务不可用");
            return -7;
        }
        result_message(result, 0, 0, "预览视口已生效");
        return 0;

    case IPCAM_CONTROL_SET_BACKLIGHT:
        if (cmd->percent < 10 || cmd->percent > 100 || !ctx->display ||
            !ipcam_display_backlight_available(ctx->display)) {
            result_message(result, -8, 0, "背光硬件不可用或保存失败");
            return -8;
        }
        {
            int old_percent = ipcam_param_get_backlight_percent();
            if (ipcam_display_set_backlight(ctx->display, cmd->percent) != 0) {
                result_message(result, -8, 0, "背光硬件不可用或保存失败");
                return -8;
            }
            if (ipcam_param_set_backlight_percent((uint8_t)cmd->percent) != 0) {
                /* 持久化失败时恢复原亮度，调用方可重试而不会看到假状态。 */
                ipcam_display_set_backlight(ctx->display, old_percent);
                result_message(result, -8, 0, "背光保存失败，已回退");
                return -8;
            }
        }
        if (ctx->screen)
            ipcam_screen_update(ctx->screen, cmd->percent,
                                ipcam_param_get_screen_timeout_min());
        if (ctx->display && !ctx->display->backlight_available &&
            ctx->display->fb_blank_available)
            result_message(result, 0, 1, "屏幕仅支持亮灭，亮度配置已保存");
        else
            result_message(result, 0, 1, "背光已生效并保存");
        return 0;

    case IPCAM_CONTROL_SET_LIGHT:
        /* 补光属于临时状态；节点不可用或写失败时不更新成功状态。 */
        if (cmd->percent < 0 || cmd->percent > 100 ||
            ipcam_light_set_percent(cmd->percent) != 0) {
            result_message(result, -16, 0, "补光硬件不可用或写入失败");
            return -16;
        }
        result_message(result, 0, 0, "补光已生效");
        return 0;

    case IPCAM_CONTROL_SET_SCREEN_TIMEOUT:
        if (!ctx->screen) {
            result_message(result, -10, 0, "熄屏服务不可用");
            return -10;
        }
        if (cmd->timeout_min != 0 && cmd->timeout_min != 1 &&
            cmd->timeout_min != 3 && cmd->timeout_min != 5 && cmd->timeout_min != 10) {
            result_message(result, -9, 0, "熄屏时间不合法");
            return -9;
        }
        if (ipcam_param_set_screen_timeout_min((uint8_t)cmd->timeout_min) != 0) {
            result_message(result, -10, 0, "熄屏时间保存失败");
            return -10;
        }
        if (ctx->screen)
            ipcam_screen_update(ctx->screen, ipcam_param_get_backlight_percent(),
                                cmd->timeout_min);
        result_message(result, 0, 1, "熄屏时间已生效并保存");
        return 0;

    case IPCAM_CONTROL_RECORD_START:
        if (!ctx->recorder || ipcam_record_request_start(ctx->recorder) != 0) {
            result_message(result, -11, 0, "录像未能受理");
            return -11;
        }
        result_message(result, 0, 0, "录像启动请求已受理");
        return 0;

    case IPCAM_CONTROL_RECORD_STOP:
        if (!ctx->recorder || ipcam_record_request_stop(ctx->recorder) != 0) {
            result_message(result, -12, 0, "录像停止请求无效");
            return -12;
        }
        result_message(result, 0, 0, "录像停止请求已受理");
        return 0;

    case IPCAM_CONTROL_PHOTO:
        if (!ctx->recorder || ipcam_record_save_photo(ctx->recorder,
                                                       result->path,
                                                       sizeof(result->path)) != 0) {
            result_message(result, -13, 0, "拍照失败，请检查有效帧、SD 卡和空间");
            return -13;
        }
        result_message(result, 0, 0, "照片已写入 SD 卡");
        return 0;

    case IPCAM_CONTROL_FORMAT_STORAGE: {
        if (!ctx->recorder) {
            result_message(result, -17, 0, "存储服务不可用");
            return -17;
        }
        /* 录像文件仍可能持有 SD 卡文件描述符；格式化前必须由用户先停止
         * 录像，避免 mkfs 与 AVI 写入并发破坏文件系统。 */
        if (recording_locks_video(ctx)) {
            result_message(result, -18, 0, "录像期间不能格式化 SD 卡");
            return -18;
        }
        char message[128] = "";
        int rc = ipcam_storage_format(ctx->recorder->storage_root,
                                      message, sizeof(message));
        result_message(result, rc == 0 ? 0 : -19, 0,
                       message[0] ? message : "SD 卡格式化失败");
        return rc == 0 ? 0 : -19;
    }

    default:
        result_message(result, -1, 0, "未知控制命令");
        return -1;
    }
}

/* 初始化统一控制上下文；不创建额外线程，命令在提交者线程串行执行。 */
int ipcam_control_init(ipcam_control_ctx_t *ctx,
                       ipcam_capture_ctx_t *capture,
                       ipcam_record_ctx_t *recorder,
                       ipcam_display_ctx_t *display,
                       ipcam_screen_ctx_t *screen,
                       ipcam_ring_buffer_t *jpeg_live_rb,
                       volatile sig_atomic_t *running)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    if (pthread_mutex_init(&ctx->mtx, NULL) != 0) return -1;
    if (pthread_mutex_init(&ctx->status_mtx, NULL) != 0) {
        pthread_mutex_destroy(&ctx->mtx);
        return -1;
    }
    ctx->capture = capture;
    ctx->recorder = recorder;
    ctx->display = display;
    ctx->screen = screen;
    ctx->jpeg_live_rb = jpeg_live_rb;
    ctx->running = running;
    ctx->next_request_id = 0;
    ctx->initialized = 1;
    /* 首次强制刷新只做一次低频探测，之后 HTTP 读取永远只复制缓存。 */
    (void)ipcam_control_refresh_status(ctx, 1);
    MLOGI("control service ready: capture=%s display=%s screen=%s recorder=%s\n",
          capture ? "yes" : "no", display ? "yes" : "no",
          screen ? "yes" : "no", recorder ? "yes" : "no");
    return 0;
}

/* 解除服务引用并销毁命令结果锁；调用前须先停止 HTTP 客户端。 */
void ipcam_control_deinit(ipcam_control_ctx_t *ctx)
{
    if (!ctx || !ctx->initialized) return;
    MLOGI("control service stopping\n");
    pthread_mutex_destroy(&ctx->status_mtx);
    pthread_mutex_destroy(&ctx->mtx);
    ctx->initialized = 0;
    MLOGI("control service stopped\n");
}

/* 返回当前驱动协商值覆盖后的已验证能力清单。 */
int ipcam_control_get_capabilities(ipcam_control_ctx_t *ctx,
                                   ipcam_control_capabilities_t *out)
{
    if (!ctx || !ctx->initialized || !out) return -1;
    fill_capabilities(ctx, out);
    overlay_capture_capability(ctx, out);
    return 0;
}

/* 组合参数、队列、网络和存储快照，任何单项探测失败均如实保留为不可用。 */
int ipcam_control_get_status(ipcam_control_ctx_t *ctx,
                             ipcam_control_status_t *out)
{
    if (!ctx || !ctx->initialized || !out) return -1;
    pthread_mutex_lock(&ctx->status_mtx);
    if (!ctx->status_cache_valid) {
        pthread_mutex_unlock(&ctx->status_mtx);
        return -1;
    }
    *out = ctx->status_cache;
    pthread_mutex_unlock(&ctx->status_mtx);
    return 0;
}

/*
 * 按固定周期更新状态缓存。快速服务计数每秒刷新，网络和存储分别采用
 * 2 秒/5 秒节流；force 只用于启动或明确要求立即反映外部状态的场景。
 */
int ipcam_control_refresh_status(ipcam_control_ctx_t *ctx, int force)
{
    if (!ctx || !ctx->initialized) return -1;
    uint64_t now = control_now_ns();
    ipcam_control_status_t next;
    int network_due, storage_due;
    uint64_t last_network, last_storage;

    pthread_mutex_lock(&ctx->status_mtx);
    next = ctx->status_cache;
    last_network = ctx->last_network_probe_ns;
    last_storage = ctx->last_storage_probe_ns;
    network_due = force || last_network == 0 ||
                  (now && now - last_network >= 2000000000ULL);
    storage_due = force || last_storage == 0 ||
                  (now && now - last_storage >= 5000000000ULL);
    pthread_mutex_unlock(&ctx->status_mtx);

    fill_fast_status(ctx, &next);
    if (network_due) {
        fill_network_status(&next);
        last_network = now;
    }
    if (storage_due) {
        fill_storage_status(&next, ctx->recorder);
        last_storage = now;
    }

    pthread_mutex_lock(&ctx->status_mtx);
    ctx->status_cache = next;
    ctx->last_network_probe_ns = last_network;
    ctx->last_storage_probe_ns = last_storage;
    ctx->status_cache_valid = 1;
    pthread_mutex_unlock(&ctx->status_mtx);
    return 0;
}

void ipcam_control_set_encoder(ipcam_control_ctx_t *ctx, ipcam_encode_ctx_t *encoder)
{
    if (!ctx || !ctx->initialized) return;
    pthread_mutex_lock(&ctx->status_mtx);
    ctx->encoder = encoder;
    pthread_mutex_unlock(&ctx->status_mtx);
}

/* 分配有界 request_id 并在锁内执行校验；返回 0 仅表示请求记录成功。 */
int ipcam_control_submit_command(ipcam_control_ctx_t *ctx,
                                 const ipcam_control_command_t *command,
                                 uint64_t *request_id)
{
    if (!ctx || !ctx->initialized || !command) return -1;
    pthread_mutex_lock(&ctx->mtx);
    uint64_t id = ++ctx->next_request_id;
    size_t slot = ctx->result_cursor++ % (sizeof(ctx->results) / sizeof(ctx->results[0]));
    ipcam_control_result_t *result = &ctx->results[slot];
    memset(result, 0, sizeof(*result));
    result->request_id = id;
    result->state = IPCAM_CONTROL_RESULT_PROCESSING;
    pthread_mutex_unlock(&ctx->mtx);
    control_log_command(command, id);

    /* 命令本体在控制器锁内串行执行；录像启停仍由录像线程异步收尾。 */
    ipcam_control_result_t completed;
    memset(&completed, 0, sizeof(completed));
    completed.request_id = id;
    int apply_rc;
    pthread_mutex_lock(&ctx->mtx);
    apply_rc = apply_command(ctx, command, &completed);
    result = find_result_locked(ctx, id);
    if (result) *result = completed;
    pthread_mutex_unlock(&ctx->mtx);
    /* 参数、视图或录像状态在下一次 1 Hz 刷新时更新；保留上一份完整快照，
     * 避免控制请求刚返回时 HTTP 状态接口短暂变成 503。 */
    if (apply_rc == 0) {
        MLOGI("command done id=%llu name=%s persisted=%d message=%s path=%s\n",
              (unsigned long long)id, control_command_name(command->type),
              completed.persisted, completed.message,
              completed.path[0] ? completed.path : "-");
    } else {
        MLOGW("command failed id=%llu name=%s rc=%d message=%s\n",
              (unsigned long long)id, control_command_name(command->type),
              apply_rc, completed.message);
    }
    if (request_id) *request_id = id;
    return 0;
}

/* 查询最近 16 个请求的结果；过期编号返回 -1，避免无限增长内存。 */
int ipcam_control_get_command_result(ipcam_control_ctx_t *ctx,
                                     uint64_t request_id,
                                     ipcam_control_result_t *out)
{
    if (!ctx || !ctx->initialized || !out || request_id == 0) return -1;
    pthread_mutex_lock(&ctx->mtx);
    ipcam_control_result_t *result = find_result_locked(ctx, request_id);
    if (!result) {
        pthread_mutex_unlock(&ctx->mtx);
        return -1;
    }
    *out = *result;
    pthread_mutex_unlock(&ctx->mtx);
    return 0;
}

/* 复制最新 JPEG 快照，调用方拥有 out_data 生命周期且无需释放 ring 槽。 */
int ipcam_control_preview_acquire(ipcam_control_ctx_t *ctx,
                                  void *out_data, size_t out_cap,
                                  ipcam_frame_t *out_frame)
{
    if (!ctx || !ctx->initialized) return -1;
    /* 正常板端返回已经完成裁剪/缩放的 RGB565；无 LCD 的 display-only
     * 进程才回退到 JPEG 快照，避免把网络编码帧误当成本地预览输出。 */
    if (ctx->display)
        return ipcam_display_preview_acquire(ctx->display, out_data, out_cap, out_frame);
    if (!ctx->jpeg_live_rb) return -1;
    return ipcam_ring_copy_latest(ctx->jpeg_live_rb, out_data, out_cap,
                                  out_frame, 0);
}

void ipcam_control_preview_release(ipcam_control_ctx_t *ctx, ipcam_frame_t *frame)
{
    /* 当前 acquire 已复制到调用方 buffer；保留 release 作为未来零拷贝扩展点。 */
    if (ctx && ctx->display) ipcam_display_preview_release(ctx->display, frame);
}
