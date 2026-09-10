#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* BCF2 用源文件模块名区分业务日志；LVGL 日志归入独立模块。 */
#define IPCAM_LOG_MODULE "LVGL"

#include "ipcam_lvgl.h"

#include "ipcam_light.h"
#include "ipcam_log.h"
#include "ipcam_param.h"
#include "ipcam_ui_fonts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipcam_config.h"

/* LVGL tick 使用单调时钟；不能使用 wall clock，否则超时会因校时跳变。 */
static uint32_t lvgl_tick_cb(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)ts.tv_nsec / 1000000ULL);
}

/* LVGL tick 只有毫秒精度；性能窗口使用独立的纳秒单调时钟，避免短 flush
 * 样本被全部量化为 0，也避免 32 位 tick 回绕影响时延统计。 */
static uint64_t lvgl_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* 休眠状态会被 LVGL 线程写入、主线程 status 查询读取，统一经过 stats_mtx
 * 访问，避免在低频状态查询时与页面边沿切换发生 C 数据竞争。 */
static void lvgl_sleep_flags_get(ipcam_lvgl_ctx_t *ctx, int *fast_path,
                                 int *fast_active)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    if (fast_path) *fast_path = ctx->sleep_fast_path;
    if (fast_active) *fast_active = ctx->sleep_fast_active;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/* 启动阶段和运行阶段都使用同一发布路径，保证 getter 看到完整的状态变化。 */
static void lvgl_sleep_path_set(ipcam_lvgl_ctx_t *ctx, int enabled)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    ctx->sleep_fast_path = enabled ? 1 : 0;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

static void lvgl_sleep_active_set(ipcam_lvgl_ctx_t *ctx, int active)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stats_mtx);
    ctx->sleep_fast_active = active ? 1 : 0;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/* 休眠提示只在固定内存路径中做 RGB565 查表，避免每帧执行全屏 alpha 混合。 */
static uint16_t sleep_darken_pixel(uint16_t pixel, const uint16_t *lut)
{
    return lut ? lut[pixel] : pixel;
}

/* 初始化与 LVGL image descriptor 配套的 RGB565 元数据；数据地址长期稳定。 */
static void lvgl_init_rgb565_dsc(lv_image_dsc_t *dsc, int width, int height,
                                 uint16_t *data)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = (uint16_t)width;
    dsc->header.h = (uint16_t)height;
    dsc->header.stride = (uint32_t)width * sizeof(uint16_t);
    dsc->data_size = (uint32_t)((size_t)width * (size_t)height * sizeof(uint16_t));
    dsc->data = (const uint8_t *)data;
}

/* 构造一次性 30% 亮度 LUT；65536 项固定分配换取休眠期间 O(1) 预暗。 */
static void lvgl_build_sleep_lut(uint16_t *lut)
{
    if (!lut) return;
    for (uint32_t value = 0; value <= UINT16_MAX; value++) {
        unsigned r = (value >> 11) & 0x1fU;
        unsigned g = (value >> 5) & 0x3fU;
        unsigned b = value & 0x1fU;
        r = (r * 3U + 5U) / 10U;
        g = (g * 3U + 5U) / 10U;
        b = (b * 3U + 5U) / 10U;
        lut[value] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

/*
 * 休眠快速路径的额外缓冲在 LVGL 启动时一次性申请。失败只关闭快速合成，
 * 保留原有半透明提示并明确记录 sleep_fast_path=0，不能把验收失败隐藏掉。
 */
static int lvgl_prepare_sleep_fast_path(ipcam_lvgl_ctx_t *ctx,
                                        ipcam_display_ctx_t *display)
{
    if (!ctx || !display || display->out_w != IPCAM_UI_SCREEN_WIDTH ||
        display->out_h != IPCAM_UI_SCREEN_HEIGHT) {
        MLOGW("sleep_fast_path=0 reason=display_size_not_800x480\n");
        return -1;
    }
    size_t bg_pixels = (size_t)display->out_w * (size_t)display->out_h;
    size_t video_pixels = (size_t)760U * 368U;
    if (bg_pixels > SIZE_MAX / sizeof(uint16_t) ||
        video_pixels > SIZE_MAX / sizeof(uint16_t)) {
        MLOGW("sleep_fast_path=0 reason=buffer_size_overflow\n");
        return -1;
    }
    ctx->sleep_bg_size = bg_pixels * sizeof(uint16_t);
    ctx->sleep_video_size = video_pixels * sizeof(uint16_t);
    ctx->sleep_bg_buf = malloc(ctx->sleep_bg_size);
    ctx->sleep_video_buf = malloc(ctx->sleep_video_size);
    ctx->sleep_lut = malloc(65536U * sizeof(uint16_t));
    if (!ctx->sleep_bg_buf || !ctx->sleep_video_buf || !ctx->sleep_lut) {
        MLOGW("sleep_fast_path=0 reason=alloc bg=%zu video=%zu lut=%zu\n",
              ctx->sleep_bg_size, ctx->sleep_video_size,
              65536U * sizeof(uint16_t));
        free(ctx->sleep_bg_buf);
        free(ctx->sleep_video_buf);
        free(ctx->sleep_lut);
        ctx->sleep_bg_buf = NULL;
        ctx->sleep_video_buf = NULL;
        ctx->sleep_lut = NULL;
        ctx->sleep_bg_size = 0;
        ctx->sleep_video_size = 0;
        return -1;
    }
    lvgl_build_sleep_lut(ctx->sleep_lut);
    lvgl_init_rgb565_dsc(&ctx->sleep_bg_dsc, display->out_w, display->out_h,
                         ctx->sleep_bg_buf);
    /* video descriptor 的尺寸在每次新预览帧到来时更新，但 data 地址不变。 */
    lvgl_init_rgb565_dsc(&ctx->sleep_video_dsc, 760, 368, ctx->sleep_video_buf);
    lvgl_sleep_path_set(ctx, 1);
    MLOGI("sleep_fast_path=1 buffers=%zu+%zu+%zu\n",
          ctx->sleep_bg_size, ctx->sleep_video_size,
          65536U * sizeof(uint16_t));
    return 0;
}

/* 把已复制的可见 framebuffer 预暗成不透明 backdrop；只在提示边沿调用。 */
static int lvgl_build_sleep_backdrop(ipcam_lvgl_ctx_t *ctx)
{
    int fast_path = 0;
    lvgl_sleep_flags_get(ctx, &fast_path, NULL);
    if (!ctx || !fast_path || !ctx->sleep_bg_buf || !ctx->display) return -1;
    if (ipcam_display_snapshot_visible(ctx->display, ctx->sleep_bg_buf,
                                       ctx->sleep_bg_size) != 0)
        return -1;
    size_t pixels = ctx->sleep_bg_size / sizeof(uint16_t);
    for (size_t i = 0; i < pixels; i++)
        ctx->sleep_bg_buf[i] = sleep_darken_pixel(ctx->sleep_bg_buf[i], ctx->sleep_lut);
    return 0;
}

/* 把当前普通预览副本预暗到提示层；只复制视频矩形，不触碰底层页面。 */
static int lvgl_build_sleep_video(ipcam_lvgl_ctx_t *ctx, uint16_t width,
                                  uint16_t height, uint32_t stride)
{
    int fast_path = 0;
    lvgl_sleep_flags_get(ctx, &fast_path, NULL);
    if (!ctx || !fast_path || !ctx->video_buf ||
        !ctx->sleep_video_buf || width == 0 || height == 0 ||
        stride < (uint32_t)width * sizeof(uint16_t) ||
        (size_t)width * height * sizeof(uint16_t) > ctx->sleep_video_size)
        return -1;
    size_t source_stride = stride;
    size_t row_bytes = (size_t)width * sizeof(uint16_t);
    for (uint16_t y = 0; y < height; y++) {
        const uint16_t *source = (const uint16_t *)(ctx->video_buf +
                                                    (size_t)y * source_stride);
        uint16_t *target = ctx->sleep_video_buf + (size_t)y * width;
        for (uint16_t x = 0; x < width; x++)
            target[x] = sleep_darken_pixel(source[x], ctx->sleep_lut);
    }
    ctx->sleep_video_dsc.header.w = width;
    ctx->sleep_video_dsc.header.h = height;
    /* 预暗层按紧凑行存储，避免把源帧可能存在的 line padding 带入 image。 */
    ctx->sleep_video_dsc.header.stride = (uint32_t)row_bytes;
    ctx->sleep_video_dsc.data_size = (uint32_t)((size_t)height * row_bytes);
    return 0;
}

/* 仅在 screen_sleep_prompt 的 0→1/1→0 边沿操作 backdrop，避免每帧整屏重绘。 */
static int lvgl_apply_sleep_state(ipcam_lvgl_ctx_t *ctx,
                                  const ipcam_ui_state_t *state)
{
    if (!ctx || !ctx->ui || !state) return 0;
    int fast_path = 0;
    int fast_active = 0;
    lvgl_sleep_flags_get(ctx, &fast_path, &fast_active);
    int prompt = state->screen_sleep_prompt && !state->screen_sleeping;
    if (!prompt) {
        if (fast_active) {
            ipcam_ui_sleep_fast_set(ctx->ui, 0, NULL, NULL);
            lvgl_sleep_active_set(ctx, 0);
            /* 退出提示时让当前普通视频 descriptor 在下一轮重新可见。 */
            ctx->last_rendered = UINT64_MAX;
        }
        return 0;
    }
    if (fast_active || !fast_path) return 0;
    if (lvgl_build_sleep_backdrop(ctx) != 0) {
        MLOGW("sleep_fast_path=0 reason=visible_snapshot_failed\n");
        lvgl_sleep_path_set(ctx, 0);
        return 0;
    }
    const lv_image_dsc_t *video = NULL;
    if (ctx->video_valid && lvgl_build_sleep_video(ctx, ctx->video_dsc.header.w,
                                                   ctx->video_dsc.header.h,
                                                   ctx->video_dsc.header.stride) == 0)
        video = &ctx->sleep_video_dsc;
    ipcam_ui_sleep_fast_set(ctx->ui, 1, &ctx->sleep_bg_dsc, video);
    lvgl_sleep_active_set(ctx, 1);
    return 1;
}

/* 结果由主线程写入、由 LVGL 线程读取；禁止跨线程直接改 label。 */
static void lvgl_set_feedback(ipcam_lvgl_ctx_t *ctx, const char *message,
                              ipcam_ui_message_severity_t severity)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->feedback_mtx);
    snprintf(ctx->feedback, sizeof(ctx->feedback), "%s", message ? message : "");
    ctx->feedback_severity = severity;
    pthread_mutex_unlock(&ctx->feedback_mtx);
}

static int lvgl_action_can_coalesce(ipcam_ui_action_type_t type)
{
    return type == IPCAM_UI_ACTION_SET_BACKLIGHT ||
           type == IPCAM_UI_ACTION_SET_SCREEN_TIMEOUT ||
           type == IPCAM_UI_ACTION_APPLY_MIRROR ||
           type == IPCAM_UI_ACTION_RECORD_START ||
           type == IPCAM_UI_ACTION_RECORD_STOP ||
           type == IPCAM_UI_ACTION_PHOTO;
}

/* 触摸按下后立即给出稳定短提示；最终控制结果会在主线程执行后覆盖它。 */
static const char *lvgl_action_pending_text(ipcam_ui_action_type_t type)
{
    switch (type) {
    case IPCAM_UI_ACTION_APPLY_VIDEO: return "视频设置正在应用";
    case IPCAM_UI_ACTION_APPLY_MIRROR: return "翻转设置正在应用";
    case IPCAM_UI_ACTION_SET_BACKLIGHT: return "亮度设置正在应用";
    case IPCAM_UI_ACTION_SET_SCREEN_TIMEOUT: return "自动熄屏设置正在应用";
    case IPCAM_UI_ACTION_RECORD_START: return "录像启动请求已提交";
    case IPCAM_UI_ACTION_RECORD_STOP: return "录像停止请求已提交";
    case IPCAM_UI_ACTION_PHOTO: return "正在保存照片";
    case IPCAM_UI_ACTION_STORAGE_FORMAT_CONFIRM: return "正在格式化 SD 卡，请稍候";
    default: return NULL;
    }
}

/*
 * 提交 LVGL 的 RGB565 buffer。
 * 双 framebuffer 模式下 color_p 就是待显示的虚拟页，只有最后一个 dirty area
 * 完成后才调用 FBIOPAN_DISPLAY，mxsfb 会在下一次 VSYNC 切页；不支持双页时
 * 才退回逐行 memcpy 的兼容路径。
 */
static void lvgl_flush_cb(lv_display_t *lv_display, const lv_area_t *area,
                          uint8_t *color_p)
{
    ipcam_lvgl_ctx_t *ctx = lv_display_get_driver_data(lv_display);
    if (!ctx || !ctx->display || !ctx->display->fb_base || !area || !color_p) {
        lv_display_flush_ready(lv_display);
        return;
    }

    ipcam_display_ctx_t *display = ctx->display;
    if (ctx->framebuffer_direct) {
        int page = -1;
        for (int candidate = 0; candidate < 2; candidate++) {
            if (color_p == ipcam_display_framebuffer_page(display, candidate)) {
                page = candidate;
                break;
            }
        }
        if (page < 0) {
            /* DIRECT 模式必须收到虚拟页基址；未知指针不能误切当前显示页。 */
            MLOGW("LVGL direct flush received unknown framebuffer=%p\n",
                  (void *)color_p);
        }
        /* DIRECT 模式可能分成多个区域，前面的区域只需完成回调，不能提前切页。 */
        if (!lv_display_flush_is_last(lv_display)) {
            lv_display_flush_ready(lv_display);
            return;
        }
        if (page >= 0 && ipcam_display_present_page(display, page) != 0) {
            /* 不能用整页 memcpy 替代失败的 VSYNC 换页，否则扫描头会看到半页新
             * 半页旧数据。宁可保持上一页，待下一次 pan 重试，也不主动制造撕裂。 */
            MLOGW("LVGL direct flush keeps visible page after pan failure: page=%d\n",
                  page);
        }
        lv_display_flush_ready(lv_display);
        return;
    }

    int32_t source_width = lv_area_get_width(area);
    if (source_width <= 0) {
        lv_display_flush_ready(lv_display);
        return;
    }
    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2;
    int32_t y2 = area->y2;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= display->out_w) x2 = display->out_w - 1;
    if (y2 >= display->out_h) y2 = display->out_h - 1;
    if (x1 > x2 || y1 > y2) {
        lv_display_flush_ready(lv_display);
        return;
    }

    size_t source_stride = (size_t)source_width * sizeof(uint16_t);
    size_t source_x_offset = (size_t)(x1 - area->x1) * sizeof(uint16_t);
    const uint8_t *source = color_p +
                            (size_t)(y1 - area->y1) * source_stride +
                            source_x_offset;
    if (ipcam_display_blit_area(display, x1, y1, x2, y2, source) != 0)
        MLOGW("LVGL fallback blit failed: area=%d,%d-%d,%d\n", x1, y1, x2, y2);
    /* 当前 flush 是同步 memcpy，回调返回前必须明确告知 LVGL 已完成。 */
    lv_display_flush_ready(lv_display);
}

/* LVGL 只在自己的线程调用 read_cb；这里不直接读取 Linux 输入 fd。 */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ipcam_lvgl_ctx_t *ctx = lv_indev_get_user_data(indev);
    if (!ctx || !data) return;
    pthread_mutex_lock(&ctx->input_mtx);
    data->point.x = (lv_coord_t)ctx->touch_x;
    data->point.y = (lv_coord_t)ctx->touch_y;
    data->state = ctx->touch_pressed ? LV_INDEV_STATE_PRESSED :
                                        LV_INDEV_STATE_RELEASED;
    pthread_mutex_unlock(&ctx->input_mtx);
    data->continue_reading = false;
}

/* UI 事件只排队，避免 LVGL 事件线程被参数落盘、录像或拍照阻塞。 */
static int lvgl_ui_action_cb(const ipcam_ui_action_t *action, void *opaque)
{
    ipcam_lvgl_ctx_t *ctx = opaque;
    if (!ctx || !action) return -1;
    pthread_mutex_lock(&ctx->action_mtx);
    if (action->type == IPCAM_UI_ACTION_PHOTO && ctx->photo_inflight) {
        /* 一张照片仍在等 JPEG 或落盘时，重复点击不能再启动第二个等待者；
         * 直接给出提示比把请求塞进队列后延迟数秒更容易让用户判断结果。 */
        pthread_mutex_unlock(&ctx->action_mtx);
        lvgl_set_feedback(ctx, "上一张照片仍在保存，请稍候",
                          IPCAM_UI_MESSAGE_WARNING);
        return 0;
    }
    /* 滑块、翻转和拍照在一个触摸周期可能产生重复动作；同类新值覆盖旧值，
     * 把队列空间留给真正需要逐条执行的导航和格式化确认动作。 */
    if (lvgl_action_can_coalesce(action->type) && ctx->action_count > 0) {
        for (size_t n = ctx->action_count; n > 0; n--) {
            size_t index = (ctx->action_head + n - 1) % IPCAM_LVGL_ACTION_QUEUE_DEPTH;
            if (ctx->action_queue[index].type != action->type) continue;
            ctx->action_queue[index] = *action;
            if (action->type == IPCAM_UI_ACTION_RECORD_START ||
                action->type == IPCAM_UI_ACTION_RECORD_STOP)
                ctx->pending_record_action = action->type;
            pthread_mutex_unlock(&ctx->action_mtx);
            const char *pending = lvgl_action_pending_text(action->type);
            if (pending) lvgl_set_feedback(ctx, pending, IPCAM_UI_MESSAGE_INFO);
            return 0;
        }
    }
    if (ctx->action_count >= IPCAM_LVGL_ACTION_QUEUE_DEPTH) {
        pthread_mutex_unlock(&ctx->action_mtx);
        MLOGW("UI action queue full, drop action=%d\n", (int)action->type);
        lvgl_set_feedback(ctx, "操作队列繁忙，请稍后重试", IPCAM_UI_MESSAGE_ERROR);
        return -1;
    }
    size_t tail = (ctx->action_head + ctx->action_count) %
                  IPCAM_LVGL_ACTION_QUEUE_DEPTH;
    ctx->action_queue[tail] = *action;
    ctx->action_count++;
    if (action->type == IPCAM_UI_ACTION_RECORD_START ||
        action->type == IPCAM_UI_ACTION_RECORD_STOP)
        ctx->pending_record_action = action->type;
    pthread_mutex_unlock(&ctx->action_mtx);
    const char *pending = lvgl_action_pending_text(action->type);
    if (pending) lvgl_set_feedback(ctx, pending, IPCAM_UI_MESSAGE_INFO);
    return 0;
}

static int pop_ui_action(ipcam_lvgl_ctx_t *ctx, ipcam_ui_action_t *action)
{
    if (!ctx || !action) return 0;
    pthread_mutex_lock(&ctx->action_mtx);
    if (ctx->action_count == 0) {
        pthread_mutex_unlock(&ctx->action_mtx);
        return 0;
    }
    *action = ctx->action_queue[ctx->action_head];
    ctx->action_head = (ctx->action_head + 1) % IPCAM_LVGL_ACTION_QUEUE_DEPTH;
    ctx->action_count--;
    pthread_mutex_unlock(&ctx->action_mtx);
    return 1;
}

static int submit_control_command(ipcam_control_ctx_t *control,
                                  ipcam_control_command_t *command,
                                  char *feedback, size_t feedback_sz)
{
    if (!control || !command) {
        if (feedback && feedback_sz > 0)
            snprintf(feedback, feedback_sz, "%s", "控制服务不可用");
        return -1;
    }
    uint64_t request_id = 0;
    int rc = ipcam_control_submit_command(control, command, &request_id);
    if (rc != 0) {
        if (feedback && feedback_sz > 0)
            snprintf(feedback, feedback_sz, "%s", "控制请求提交失败");
        return rc;
    }
    ipcam_control_result_t result;
    memset(&result, 0, sizeof(result));
    if (ipcam_control_get_command_result(control, request_id, &result) != 0) {
        if (feedback && feedback_sz > 0)
            snprintf(feedback, feedback_sz, "%s", "控制结果不可用");
        return -1;
    }
    if (feedback && feedback_sz > 0 && result.message[0])
        snprintf(feedback, feedback_sz, "%s", result.message);
    if (result.state == IPCAM_CONTROL_RESULT_FAILED)
        return result.error_code ? result.error_code : -1;
    if (result.state != IPCAM_CONTROL_RESULT_DONE) {
        if (feedback && feedback_sz > 0)
            snprintf(feedback, feedback_sz, "%s", "命令已受理，等待设备完成");
    }
    return 0;
}

/* 把 P04 的“应用”拆成视频规格和镜像两条既有控制契约。 */
static int submit_video_action(ipcam_control_ctx_t *control,
                               const ipcam_ui_action_t *action,
                               char *feedback, size_t feedback_sz)
{
    ipcam_control_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = IPCAM_CONTROL_SET_VIDEO;
    command.video.width = (uint16_t)action->value0;
    command.video.height = (uint16_t)action->value1;
    command.video.target_fps = (uint8_t)action->value2;
    command.video.jpeg_quality = (uint8_t)action->value3;
    int rc = submit_control_command(control, &command, feedback, feedback_sz);
    if (rc != 0) return rc;
    memset(&command, 0, sizeof(command));
    command.type = IPCAM_CONTROL_SET_MIRROR;
    command.mirror_horizontal = action->value4 ? 1 : 0;
    command.mirror_vertical = action->value5 ? 1 : 0;
    return submit_control_command(control, &command, feedback, feedback_sz);
}

/* 在主线程执行一个动作；硬件和文件访问集中在这里便于串行审计。 */
static int process_one_ui_action(ipcam_control_ctx_t *control,
                                 const ipcam_ui_action_t *action,
                                 char *feedback, size_t feedback_sz)
{
    if (!control || !action) return -1;
    ipcam_control_command_t command;
    memset(&command, 0, sizeof(command));
    switch (action->type) {
    case IPCAM_UI_ACTION_APPLY_VIDEO:
        return submit_video_action(control, action, feedback, feedback_sz);
    case IPCAM_UI_ACTION_APPLY_MIRROR:
        command.type = IPCAM_CONTROL_SET_MIRROR;
        command.mirror_horizontal = action->value0 ? 1 : 0;
        command.mirror_vertical = action->value1 ? 1 : 0;
        break;
    case IPCAM_UI_ACTION_SET_PREVIEW:
        command.type = IPCAM_CONTROL_SET_PREVIEW;
        command.enabled = action->value0 ? 1 : 0;
        break;
    case IPCAM_UI_ACTION_SET_LIGHT:
        command.type = IPCAM_CONTROL_SET_LIGHT;
        command.percent = action->value0;
        break;
    case IPCAM_UI_ACTION_SET_BACKLIGHT:
        command.type = IPCAM_CONTROL_SET_BACKLIGHT;
        command.percent = action->value0;
        break;
    case IPCAM_UI_ACTION_SET_SCREEN_TIMEOUT:
        command.type = IPCAM_CONTROL_SET_SCREEN_TIMEOUT;
        command.timeout_min = action->value0;
        break;
    case IPCAM_UI_ACTION_SCREEN_KEEP_AWAKE:
        if (!control->screen) {
            if (feedback && feedback_sz > 0)
                snprintf(feedback, feedback_sz, "%s", "熄屏服务不可用");
            return -1;
        }
        if (ipcam_screen_keep_awake(control->screen) != 0) {
            if (feedback && feedback_sz > 0)
                snprintf(feedback, feedback_sz, "%s", "继续显示失败");
            return -1;
        }
        if (feedback && feedback_sz > 0)
            snprintf(feedback, feedback_sz, "%s", "已继续显示");
        return 0;
    case IPCAM_UI_ACTION_SCREEN_SLEEP:
        if (!control->screen) {
            if (feedback && feedback_sz > 0)
                snprintf(feedback, feedback_sz, "%s", "熄屏服务不可用");
            return -1;
        }
        if (ipcam_screen_request_sleep(control->screen) != 0) {
            if (feedback && feedback_sz > 0)
                snprintf(feedback, feedback_sz, "%s", "立即熄屏失败");
            return -1;
        }
        if (feedback && feedback_sz > 0)
            snprintf(feedback, feedback_sz, "%s", "屏幕已休眠");
        return 0;
    case IPCAM_UI_ACTION_RECORD_START:
        command.type = IPCAM_CONTROL_RECORD_START;
        break;
    case IPCAM_UI_ACTION_RECORD_STOP:
        command.type = IPCAM_CONTROL_RECORD_STOP;
        break;
    case IPCAM_UI_ACTION_PHOTO:
        command.type = IPCAM_CONTROL_PHOTO;
        break;
    case IPCAM_UI_ACTION_RESET_VIEW: {
        int enabled = 1;
        if (control->display) {
            float zoom = 1.0f, center_x = 0.5f, center_y = 0.5f;
            ipcam_display_get_view(control->display, &enabled, &zoom,
                                   &center_x, &center_y);
        }
        command.type = IPCAM_CONTROL_SET_VIEW;
        command.enabled = enabled;
        command.zoom = 1.0f;
        command.center_x = 0.5f;
        command.center_y = 0.5f;
        break;
    }
    case IPCAM_UI_ACTION_STORAGE_FORMAT_CONFIRM:
        command.type = IPCAM_CONTROL_FORMAT_STORAGE;
        break;
    default:
        /* 导航和设置选择已经在 UI 线程本地处理，不应进入 control。 */
        return 0;
    }
    return submit_control_command(control, &command, feedback, feedback_sz);
}

static ipcam_ui_record_state_t map_record_state(ipcam_record_state_t state)
{
    switch (state) {
    case IPCAM_RECORD_STARTING: return IPCAM_UI_RECORD_STARTING;
    case IPCAM_RECORD_RECORDING: return IPCAM_UI_RECORDING;
    case IPCAM_RECORD_STOPPING: return IPCAM_UI_RECORD_STOPPING;
    case IPCAM_RECORD_ERROR: return IPCAM_UI_RECORD_ERROR;
    case IPCAM_RECORD_IDLE:
    default: return IPCAM_UI_RECORD_IDLE;
    }
}

/*
 * 主线程每秒才处理一次动作，而录像线程又要等首帧才进入 RECORDING。
 * 在这个窗口里保留 STARTING/STOPPING 的视觉状态，避免下一轮快照把按钮
 * 恢复成旧状态，用户再次点击后形成重复开始或重复停止请求。
 */
static void lvgl_apply_pending_record_state(ipcam_lvgl_ctx_t *ctx,
                                            ipcam_record_state_t actual,
                                            ipcam_ui_state_t *state)
{
    if (!ctx || !state) return;
    pthread_mutex_lock(&ctx->action_mtx);
    if (ctx->pending_record_action == IPCAM_UI_ACTION_RECORD_START) {
        if (actual == IPCAM_RECORD_STARTING || actual == IPCAM_RECORD_RECORDING ||
            actual == IPCAM_RECORD_ERROR) {
            ctx->pending_record_action = (ipcam_ui_action_type_t)0;
        } else {
            state->record_state = IPCAM_UI_RECORD_STARTING;
        }
    } else if (ctx->pending_record_action == IPCAM_UI_ACTION_RECORD_STOP) {
        if (actual == IPCAM_RECORD_IDLE || actual == IPCAM_RECORD_ERROR) {
            ctx->pending_record_action = (ipcam_ui_action_type_t)0;
        } else {
            state->record_state = IPCAM_UI_RECORD_STOPPING;
        }
    }
    pthread_mutex_unlock(&ctx->action_mtx);
}

/* 将现有服务的只读快照映射到设计层，不让页面直接依赖业务头文件。 */
static void fill_ui_state(ipcam_lvgl_ctx_t *ctx, ipcam_ui_state_t *state)
{
    ipcam_ui_state_init(state);
    state->camera_ready = ctx->display != NULL;
    state->display_ready = ctx->display && ctx->display->fb_base != NULL;
    state->touch_ready = 1;
    state->video_frame_valid = ctx->video_valid ? 1 : 0;
    state->video_width = ipcam_param_get_capture_w();
    state->video_height = ipcam_param_get_capture_h();
    state->target_fps = ipcam_param_get_target_fps();
    state->jpeg_quality = ipcam_param_get_jpeg_quality();
    state->mirror_horizontal = ipcam_param_get_mirror_horizontal();
    state->mirror_vertical = ipcam_param_get_mirror_vertical();
    state->preview_enabled = ipcam_param_get_preview_enabled();
    state->backlight_percent = ipcam_param_get_backlight_percent();
    state->screen_timeout_min = ipcam_param_get_screen_timeout_min();
    state->light_percent = (uint8_t)ipcam_light_get_percent();
    state->measured_fps = ctx->measured_fps;
    state->network_link = 0;
    state->network_ip[0] = '\0';
    state->storage_mounted = 0;
    state->storage_total_bytes = 0;
    state->storage_used_bytes = 0;
    state->storage_available_bytes = 0;
    state->storage_format_supported = 0;
    state->storage_mount_path[0] = '\0';
    state->storage_device[0] = '\0';
    state->storage_fs_type[0] = '\0';
    state->record_state = IPCAM_UI_RECORD_IDLE;

    int view_enabled = 1;
    ipcam_display_get_view(ctx->display, &view_enabled, &state->zoom,
                           NULL, NULL);
    state->preview_view_enabled = view_enabled ? 1 : 0;

    pthread_mutex_lock(&ctx->service_mtx);
    ipcam_control_ctx_t *control = ctx->control;
    pthread_mutex_unlock(&ctx->service_mtx);
    if (control) {
        ipcam_control_status_t status;
        memset(&status, 0, sizeof(status));
        if (ipcam_control_get_status(control, &status) == 0) {
            if (status.video.width) state->video_width = status.video.width;
            if (status.video.height) state->video_height = status.video.height;
            if (status.video.target_fps) state->target_fps = status.video.target_fps;
            if (status.video.jpeg_quality) state->jpeg_quality = status.video.jpeg_quality;
            state->mirror_horizontal = status.mirror_horizontal;
            state->mirror_vertical = status.mirror_vertical;
            state->preview_enabled = status.preview_enabled;
            state->preview_view_enabled = status.preview_view_enabled;
            state->zoom = status.preview_zoom > 0.0f ? status.preview_zoom : 1.0f;
            state->backlight_percent = status.backlight_percent;
            state->light_percent = status.light_percent;
            state->screen_timeout_min = status.screen_timeout_min;
            state->network_link = status.network_link ? 1 : 0;
            snprintf(state->network_ip, sizeof(state->network_ip), "%s",
                     status.network_ip);
            state->storage_mounted = status.storage_mounted ? 1 : 0;
            state->storage_total_bytes = status.storage_total_bytes;
            state->storage_used_bytes = status.storage_used_bytes;
            state->storage_available_bytes = status.storage_available_bytes;
            state->storage_format_supported = status.storage_format_supported ? 1 : 0;
            snprintf(state->storage_mount_path, sizeof(state->storage_mount_path), "%s",
                     status.storage_mount_path);
            snprintf(state->storage_device, sizeof(state->storage_device), "%s",
                     status.storage_device);
            snprintf(state->storage_fs_type, sizeof(state->storage_fs_type), "%s",
                     status.storage_fs_type);
            state->record_state = map_record_state(status.record.state);
            state->record_segment_no = status.record.segment_no;
            state->record_elapsed_ms = status.record.elapsed_ms;
            state->record_frame_count = status.record.frame_count;
            lvgl_apply_pending_record_state(ctx, status.record.state, state);
            if (status.record.state == IPCAM_RECORD_ERROR &&
                status.record.last_error[0]) {
                snprintf(state->message, sizeof(state->message), "%s",
                         status.record.last_error);
                state->message_severity = IPCAM_UI_MESSAGE_ERROR;
            }
        }
        if (control->screen) {
            ipcam_screen_status_t screen_status;
            memset(&screen_status, 0, sizeof(screen_status));
            if (ipcam_screen_get_status(control->screen, &screen_status) == 0) {
                state->screen_sleep_prompt = screen_status.sleep_prompt_visible ? 1 : 0;
                state->screen_sleep_remaining_sec =
                    (uint8_t)(screen_status.sleep_remaining_sec > 255 ? 255 :
                              screen_status.sleep_remaining_sec);
                state->screen_sleeping = screen_status.sleeping ? 1 : 0;
            }
        }
    }
    if (!state->camera_ready) {
        snprintf(state->message, sizeof(state->message), "摄像头不可用");
        state->message_severity = IPCAM_UI_MESSAGE_ERROR;
    }
    /* 主线程动作结果必须在摄像头提示之后覆盖 message，否则一次拍照失败
     * 会被下一次 100ms 状态轮询抹掉，用户仍然看不到真正原因。 */
    pthread_mutex_lock(&ctx->feedback_mtx);
    state->storage_formatting = ctx->storage_formatting ? 1 : 0;
    if (ctx->feedback[0]) {
        snprintf(state->message, sizeof(state->message), "%s", ctx->feedback);
        state->message_severity = ctx->feedback_severity;
    }
    pthread_mutex_unlock(&ctx->feedback_mtx);
}

/*
 * 根据当前页面同步 display 的视频目标。
 * 页面导航在 LVGL 线程中完成，因此这里不会跨线程触碰 UI；目标改变时先使
 * 旧 descriptor 失效，等待 display 生成同尺寸帧后再重新绑定 image。
 */
static void lvgl_sync_preview_target(ipcam_lvgl_ctx_t *ctx)
{
    if (!ctx || !ctx->ui || !ctx->display) return;
    ipcam_ui_video_viewport_t viewport;
    if (ipcam_ui_get_video_viewport(ctx->ui, &viewport) != 0) return;
    ipcam_ui_screen_t screen = ipcam_ui_get_screen(ctx->ui);
    int target_w = viewport.active ? viewport.width : 0;
    int target_h = viewport.active ? viewport.height : 0;
    if (screen == ctx->preview_target_screen &&
        target_w == ctx->preview_target_w && target_h == ctx->preview_target_h)
        return;
    if (ipcam_display_set_preview_target(ctx->display, target_w, target_h) != 0) {
        MLOGW("preview target rejected: screen=%d size=%dx%d\n",
              (int)screen, target_w, target_h);
        target_w = 0;
        target_h = 0;
    }
    ctx->preview_target_screen = screen;
    ctx->preview_target_w = target_w;
    ctx->preview_target_h = target_h;
    ctx->video_valid = 0;
    /* 用哨兵强制新页面至少尝试一次取帧；取不到时再按 rendered 计数等待，
     * 避免设置页每 5ms 重复 acquire/隐藏 image，反过来消耗 LVGL CPU。 */
    ctx->last_rendered = UINT64_MAX;
    ipcam_ui_set_video_source(ctx->ui, NULL, 0);
    MLOGI("LVGL preview target: screen=%d size=%dx%d\n",
          (int)screen, target_w, target_h);
}

/* 按当前页面目标尺寸复制最新预览帧，避免 LVGL 再执行全屏 image 缩放。 */
static int lvgl_update_video(ipcam_lvgl_ctx_t *ctx)
{
    int fast_active = 0;
    lvgl_sleep_flags_get(ctx, NULL, &fast_active);
    if (!ctx->display->dynamic_video_enabled) {
        if (ctx->video_valid) {
            ctx->video_valid = 0;
            ipcam_ui_set_video_source(ctx->ui, NULL, 0);
        }
        ctx->last_rendered = UINT64_MAX;
        return 0;
    }
    int preview_enabled = ipcam_param_get_preview_enabled() ? 1 : 0;
    if (!preview_enabled || ctx->preview_target_w <= 0 || ctx->preview_target_h <= 0) {
        if (ctx->video_valid) {
            ctx->video_valid = 0;
            if (fast_active)
                ipcam_ui_sleep_fast_set(ctx->ui, 1, &ctx->sleep_bg_dsc, NULL);
            else
                ipcam_ui_set_video_source(ctx->ui, NULL, 0);
        }
        ctx->last_rendered = UINT64_MAX;
        return 0;
    }

    uint64_t rendered = 0;
    ipcam_display_get_stats(ctx->display, &rendered);
    if (rendered == ctx->last_rendered) return 0;

    ipcam_frame_t frame;
    if (ipcam_display_preview_acquire(ctx->display, ctx->video_buf,
                                      ctx->video_buf_size, &frame) != 0) {
        ctx->video_valid = 0;
        ctx->last_rendered = rendered;
        if (fast_active)
            ipcam_ui_sleep_fast_set(ctx->ui, 1, &ctx->sleep_bg_dsc, NULL);
        else
            ipcam_ui_set_video_source(ctx->ui, NULL, 0);
        return 0;
    }
    if (frame.width != (uint16_t)ctx->preview_target_w ||
        frame.height != (uint16_t)ctx->preview_target_h ||
        frame.stride != (uint32_t)ctx->preview_target_w * sizeof(uint16_t)) {
        MLOGW("LVGL preview frame mismatch: %ux%u stride=%u\n",
              frame.width, frame.height, frame.stride);
        ctx->video_valid = 0;
        ctx->last_rendered = rendered;
        if (fast_active)
            ipcam_ui_sleep_fast_set(ctx->ui, 1, &ctx->sleep_bg_dsc, NULL);
        else
            ipcam_ui_set_video_source(ctx->ui, NULL, 0);
        return 0;
    }
    ctx->video_dsc.header.w = frame.width;
    ctx->video_dsc.header.h = frame.height;
    ctx->video_dsc.header.stride = frame.stride;
    ctx->video_dsc.data_size = (uint32_t)((size_t)frame.stride * frame.height);
    /* video_dsc.data 始终指向 video_buf；复制完成后 UI 只引用该稳定地址。 */
    if (fast_active) {
        if (lvgl_build_sleep_video(ctx, frame.width, frame.height, frame.stride) == 0) {
            ipcam_ui_sleep_fast_set(ctx->ui, 1, &ctx->sleep_bg_dsc,
                                    &ctx->sleep_video_dsc);
            ipcam_ui_sleep_fast_invalidate_video(ctx->ui);
        } else {
            /* 该帧无法安全映射到提示层时不显示半成品，下一帧继续尝试。 */
            ipcam_ui_sleep_fast_set(ctx->ui, 1, &ctx->sleep_bg_dsc, NULL);
        }
    } else {
        ipcam_ui_set_video_source(ctx->ui, &ctx->video_dsc, 1);
    }
    ctx->last_rendered = rendered;
    ctx->video_valid = 1;
    return fast_active ? 2 : 1;
}

/* 每 500ms 计算一次实际 LCD 帧率，避免按 5ms LVGL tick 得到抖动数值。 */
static void update_measured_fps(ipcam_lvgl_ctx_t *ctx)
{
    uint64_t rendered = 0;
    ipcam_display_get_stats(ctx->display, &rendered);
    uint32_t now = lvgl_tick_cb();
    if (ctx->last_fps_tick == 0) {
        ctx->last_fps_tick = now;
        ctx->last_fps_frames = rendered;
        return;
    }
    uint32_t elapsed = now - ctx->last_fps_tick;
    if (elapsed < 500) return;
    ctx->measured_fps = (float)(rendered - ctx->last_fps_frames) * 1000.0f /
                        (float)elapsed;
    ctx->last_fps_tick = now;
    ctx->last_fps_frames = rendered;
}

/* LVGL 主循环线程；所有对象、页面状态和 flush 调用都在此串行执行。 */
static void *lvgl_thread(void *arg)
{
    ipcam_lvgl_ctx_t *ctx = arg;
    MLOGI("LVGL thread start, display=%dx%d design=%dx%d\n",
          ctx->display->out_w, ctx->display->out_h,
          IPCAM_UI_SCREEN_WIDTH, IPCAM_UI_SCREEN_HEIGHT);
    ctx->preview_target_screen = (ipcam_ui_screen_t)-1;
    while (*ctx->running && ctx->service_running) {
        /* pan 失败后暂缓下一次 LVGL 绘制；重试最多每秒一次，成功前不让
         * renderer 触碰仍在扫描的可见页，网络/编码/录像线程继续运行。 */
        if (ctx->framebuffer_direct && ipcam_display_pan_fault(ctx->display)) {
            (void)ipcam_display_retry_pan(ctx->display);
            if (ipcam_display_pan_fault(ctx->display)) {
                usleep(5 * 1000);
                continue;
            }
        }
        /* 页面创建/销毁必须避开 lv_timer_handler 的事件回调栈；若导航失败，
         * 也强制重绑视频源，确保恢复出来的新首页不会停留在占位图。 */
        int navigation = ipcam_ui_process_navigation(ctx->ui);
        if (navigation != 0) {
            ctx->preview_target_screen = (ipcam_ui_screen_t)-1;
            ctx->last_rendered = UINT64_MAX;
            ctx->video_valid = 0;
        }
        lvgl_sync_preview_target(ctx);
        uint64_t video_started_ns = lvgl_now_ns();
        int video_result = lvgl_update_video(ctx);
        uint64_t video_ended_ns = lvgl_now_ns();
        if (video_result != 0) {
            pthread_mutex_lock(&ctx->stats_mtx);
            if (video_ended_ns >= video_started_ns)
                ipcam_perf_window_add(&ctx->video_window,
                                      video_ended_ns - video_started_ns);
            ctx->video_frames++;
            if (video_result == 2) {
                if (video_ended_ns >= video_started_ns)
                    ipcam_perf_window_add(&ctx->sleep_window,
                                          video_ended_ns - video_started_ns);
                ctx->sleep_prompt_redraws++;
            }
            pthread_mutex_unlock(&ctx->stats_mtx);
        }
        update_measured_fps(ctx);
        /* 业务状态只需 1 Hz；视频序号仍在每次新帧到来时即时更新。 */
        uint32_t now = lvgl_tick_cb();
        if (ctx->last_ui_tick == 0 || now - ctx->last_ui_tick >= 1000) {
            ipcam_ui_state_t state;
            fill_ui_state(ctx, &state);
            uint64_t sleep_started_ns = lvgl_now_ns();
            int sleep_edge = lvgl_apply_sleep_state(ctx, &state);
            uint64_t sleep_ended_ns = lvgl_now_ns();
            if (sleep_edge) {
                pthread_mutex_lock(&ctx->stats_mtx);
                if (sleep_ended_ns >= sleep_started_ns)
                    ipcam_perf_window_add(&ctx->sleep_window,
                                          sleep_ended_ns - sleep_started_ns);
                ctx->sleep_prompt_redraws++;
                pthread_mutex_unlock(&ctx->stats_mtx);
            }
            (void)ipcam_ui_update(ctx->ui, &state);
            ctx->last_ui_tick = now;
        }
        uint64_t handler_started_ns = lvgl_now_ns();
        lv_timer_handler();
        uint64_t handler_ended_ns = lvgl_now_ns();
        pthread_mutex_lock(&ctx->stats_mtx);
        if (handler_ended_ns >= handler_started_ns)
            ipcam_perf_window_add(&ctx->handler_window,
                                  handler_ended_ns - handler_started_ns);
        ctx->handler_count++;
        pthread_mutex_unlock(&ctx->stats_mtx);
        usleep(5 * 1000);
    }
    MLOGI("LVGL thread exit\n");
    return NULL;
}

int ipcam_lvgl_start(ipcam_lvgl_ctx_t *ctx, ipcam_display_ctx_t *display,
                     volatile sig_atomic_t *running)
{
    if (!ctx || !display || !running || !display->fb_base || display->fb_bpp != 16 ||
        display->out_w <= 0 || display->out_h <= 0 ||
        display->fb_line_length < display->out_w * (int)sizeof(uint16_t)) {
        MLOGE("invalid display context for LVGL: ctx=%p display=%p fb=%p bpp=%d "
              "size=%dx%d stride=%d\n",
              (void *)ctx, (void *)display,
              display ? display->fb_base : NULL,
              display ? display->fb_bpp : 0,
              display ? display->out_w : 0, display ? display->out_h : 0,
              display ? display->fb_line_length : 0);
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->display = display;
    ctx->running = running;
    ctx->service_running = 1;
    ctx->last_rendered = 0;
    ctx->video_valid = 0;
    ctx->framebuffer_direct = display->fb_pan_enabled && display->fb_page_size > 0;
    int stats_mutex_rc = pthread_mutex_init(&ctx->stats_mtx, NULL);
    if (stats_mutex_rc != 0) {
        MLOGE("init LVGL stats mutex failed: %s (%d)\n",
              strerror(stats_mutex_rc), stats_mutex_rc);
        return -1;
    }
    ipcam_perf_window_init(&ctx->handler_window);
    ipcam_perf_window_init(&ctx->video_window);
    ipcam_perf_window_init(&ctx->sleep_window);
    int mutex_rc = pthread_mutex_init(&ctx->input_mtx, NULL);
    if (mutex_rc != 0) {
        MLOGE("init LVGL input mutex failed: %s (%d)\n", strerror(mutex_rc), mutex_rc);
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    mutex_rc = pthread_mutex_init(&ctx->action_mtx, NULL);
    if (mutex_rc != 0) {
        MLOGE("init LVGL action mutex failed: %s (%d)\n", strerror(mutex_rc), mutex_rc);
        pthread_mutex_destroy(&ctx->input_mtx);
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    mutex_rc = pthread_mutex_init(&ctx->service_mtx, NULL);
    if (mutex_rc != 0) {
        MLOGE("init LVGL service mutex failed: %s (%d)\n", strerror(mutex_rc), mutex_rc);
        pthread_mutex_destroy(&ctx->action_mtx);
        pthread_mutex_destroy(&ctx->input_mtx);
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }
    mutex_rc = pthread_mutex_init(&ctx->feedback_mtx, NULL);
    if (mutex_rc != 0) {
        MLOGE("init LVGL feedback mutex failed: %s (%d)\n", strerror(mutex_rc), mutex_rc);
        pthread_mutex_destroy(&ctx->service_mtx);
        pthread_mutex_destroy(&ctx->action_mtx);
        pthread_mutex_destroy(&ctx->input_mtx);
        pthread_mutex_destroy(&ctx->stats_mtx);
        return -1;
    }

    size_t pixels = (size_t)display->out_w * (size_t)display->out_h;
    if (pixels > SIZE_MAX / sizeof(uint16_t)) {
        MLOGE("LVGL video buffer size overflow: pixels=%zu\n", pixels);
        goto fail_mutex;
    }
    ctx->video_buf_size = pixels * sizeof(uint16_t);
    ctx->draw_buf_size = ctx->framebuffer_direct ? display->fb_page_size :
                         (size_t)display->out_w * IPCAM_LVGL_BUFFER_LINES *
                         sizeof(uint16_t);
    if (ctx->draw_buf_size == 0 || ctx->video_buf_size == 0) {
        MLOGE("invalid LVGL buffer sizes: draw=%zu video=%zu\n",
              ctx->draw_buf_size, ctx->video_buf_size);
        goto fail_mutex;
    }
    MLOGI("LVGL buffers requested: draw=%zu video=%zu mode=%s\n",
          ctx->draw_buf_size, ctx->video_buf_size,
          ctx->framebuffer_direct ? "direct-fb-pan" : "partial-copy");
    if (!ctx->framebuffer_direct) ctx->draw_buf = malloc(ctx->draw_buf_size);
    ctx->video_buf = calloc(1, ctx->video_buf_size);
    if ((!ctx->framebuffer_direct && !ctx->draw_buf) || !ctx->video_buf) {
        MLOGE("alloc LVGL buffers failed: draw=%zu video=%zu\n",
              ctx->draw_buf_size, ctx->video_buf_size);
        goto fail_buffers;
    }
    (void)lvgl_prepare_sleep_fast_path(ctx, display);

    lv_init();
    MLOGI("LVGL core initialized\n");
    lv_tick_set_cb(lvgl_tick_cb);
    ctx->lv_display = lv_display_create(display->out_w, display->out_h);
    if (!ctx->lv_display) {
        MLOGE("lv_display_create failed: size=%dx%d\n",
              display->out_w, display->out_h);
        goto fail_lvgl;
    }
    MLOGI("LVGL display object created: %dx%d\n",
          display->out_w, display->out_h);
    lv_display_set_driver_data(ctx->lv_display, ctx);
    if (ctx->framebuffer_direct) {
        void *page0 = ipcam_display_framebuffer_page(display, 0);
        void *page1 = ipcam_display_framebuffer_page(display, 1);
        if (!page0 || !page1) {
            MLOGE("LVGL direct framebuffer pages unavailable\n");
            goto fail_lvgl;
        }
        /* DIRECT 模式让两个完整页保持一致，只提交 dirty area；line_length
         * 作为 stride 传入，兼容 LCD 行尾 padding。 */
        lv_display_set_buffers_with_stride(ctx->lv_display, page0, page1,
                                           (uint32_t)ctx->draw_buf_size,
                                           (uint32_t)display->fb_line_length,
                                           LV_DISPLAY_RENDER_MODE_DIRECT);
    } else {
        lv_display_set_buffers(ctx->lv_display, ctx->draw_buf, NULL,
                               (uint32_t)ctx->draw_buf_size,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
    }
    lv_display_set_flush_cb(ctx->lv_display, lvgl_flush_cb);
    lv_display_set_default(ctx->lv_display);

    memset(&ctx->video_dsc, 0, sizeof(ctx->video_dsc));
    ctx->video_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ctx->video_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    ctx->video_dsc.header.w = (uint16_t)display->out_w;
    ctx->video_dsc.header.h = (uint16_t)display->out_h;
    ctx->video_dsc.header.stride = (uint32_t)display->out_w * sizeof(uint16_t);
    ctx->video_dsc.data_size = (uint32_t)ctx->video_buf_size;
    ctx->video_dsc.data = ctx->video_buf;

    ctx->lv_indev = lv_indev_create();
    if (!ctx->lv_indev) {
        MLOGE("lv_indev_create failed\n");
        goto fail_lvgl;
    }
    MLOGI("LVGL input object created\n");
    lv_indev_set_type(ctx->lv_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ctx->lv_indev, lvgl_touch_read_cb);
    lv_indev_set_user_data(ctx->lv_indev, ctx);
    lv_indev_set_display(ctx->lv_indev, ctx->lv_display);

    ipcam_ui_actions_t actions = { .on_action = lvgl_ui_action_cb, .opaque = ctx };
    /*
     * 原实现传入 NULL，所有中文标签都会回退到不含 CJK 字形的 Montserrat 14，
     * 结果是界面框架可见但中文区域为空。静态链接裁剪字库不依赖板端文件系统，
     * 同时保留 Montserrat 处理 LV_SYMBOL_*，避免图标因切换到中文字库而消失。
     */
    static const ipcam_ui_fonts_t ui_fonts = {
        .body = &ipcam_font_cjk_16,
        .small = &ipcam_font_cjk_14,
        .title = &ipcam_font_cjk_20,
        .icon = LV_FONT_DEFAULT,
    };
    ctx->ui = ipcam_ui_create(&actions, &ui_fonts);
    if (!ctx->ui) {
        MLOGE("ipcam_ui_create failed; see UI page diagnostics above\n");
        goto fail_lvgl;
    }
    MLOGI("IPCam UI object created\n");
    int sleep_fast_path = 0;
    lvgl_sleep_flags_get(ctx, &sleep_fast_path, NULL);
    if (sleep_fast_path &&
        ipcam_ui_sleep_fast_configure(ctx->ui, sleep_fast_path) != 0) {
        lvgl_sleep_path_set(ctx, 0);
        MLOGW("sleep_fast_path=0 reason=overlay_objects_unavailable\n");
    }
    ipcam_ui_set_video_source(ctx->ui, NULL, 0);
    if (display->out_w != IPCAM_UI_SCREEN_WIDTH ||
        display->out_h != IPCAM_UI_SCREEN_HEIGHT)
        MLOGW("LCD %dx%d differs from design %dx%d\n", display->out_w, display->out_h,
              IPCAM_UI_SCREEN_WIDTH, IPCAM_UI_SCREEN_HEIGHT);

    if (pthread_create(&ctx->thread, NULL, lvgl_thread, ctx) != 0) {
        MLOGE("pthread_create LVGL failed: %s\n", strerror(errno));
        goto fail_lvgl;
    }
    ctx->thread_started = 1;
    MLOGI("LVGL ready: fb=%dx%d RGB565, draw=%zu bytes, UI=P01..P08\n",
          display->out_w, display->out_h, ctx->draw_buf_size);
    return 0;

fail_lvgl:
    if (ctx->ui) {
        ipcam_ui_destroy(ctx->ui);
        ctx->ui = NULL;
    }
    if (lv_is_initialized()) lv_deinit();
    ctx->lv_display = NULL;
    ctx->lv_indev = NULL;
fail_buffers:
    free(ctx->sleep_lut);
    free(ctx->sleep_video_buf);
    free(ctx->sleep_bg_buf);
    ctx->sleep_lut = NULL;
    ctx->sleep_video_buf = NULL;
    ctx->sleep_bg_buf = NULL;
    free(ctx->video_buf);
    free(ctx->draw_buf);
    ctx->video_buf = NULL;
    ctx->draw_buf = NULL;
fail_mutex:
    pthread_mutex_destroy(&ctx->feedback_mtx);
    pthread_mutex_destroy(&ctx->service_mtx);
    pthread_mutex_destroy(&ctx->action_mtx);
    pthread_mutex_destroy(&ctx->input_mtx);
    pthread_mutex_destroy(&ctx->stats_mtx);
    memset(ctx, 0, sizeof(*ctx));
    return -1;
}

void ipcam_lvgl_set_control(ipcam_lvgl_ctx_t *ctx, ipcam_control_ctx_t *control)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->service_mtx);
    ctx->control = control;
    pthread_mutex_unlock(&ctx->service_mtx);
}

/* 主线程每轮取空动作；若 control 尚未发布则保留队列等待下一轮。 */
void ipcam_lvgl_process_actions(ipcam_lvgl_ctx_t *ctx)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->service_mtx);
    ipcam_control_ctx_t *control = ctx->control;
    pthread_mutex_unlock(&ctx->service_mtx);
    if (!control) return;
    ipcam_ui_action_t action;
    while (pop_ui_action(ctx, &action)) {
        if (action.type == IPCAM_UI_ACTION_PHOTO) {
            pthread_mutex_lock(&ctx->action_mtx);
            ctx->photo_inflight = 1;
            pthread_mutex_unlock(&ctx->action_mtx);
        }
        if (action.type == IPCAM_UI_ACTION_STORAGE_FORMAT_CONFIRM) {
            pthread_mutex_lock(&ctx->feedback_mtx);
            ctx->storage_formatting = 1;
            pthread_mutex_unlock(&ctx->feedback_mtx);
        }
        char feedback[128] = "";
        int rc = process_one_ui_action(control, &action, feedback,
                                       sizeof(feedback));
        if (action.type == IPCAM_UI_ACTION_STORAGE_FORMAT_CONFIRM) {
            pthread_mutex_lock(&ctx->feedback_mtx);
            ctx->storage_formatting = 0;
            pthread_mutex_unlock(&ctx->feedback_mtx);
        }
        pthread_mutex_lock(&ctx->action_mtx);
        if (action.type == IPCAM_UI_ACTION_PHOTO)
            ctx->photo_inflight = 0;
        /* 失败请求不会再有录像线程状态可供下一轮确认，必须及时解除保持态；
         * 若成功则等真实 STARTING/RECORDING 或 IDLE 状态自然清除。 */
        if (rc != 0 && ctx->pending_record_action == action.type)
            ctx->pending_record_action = (ipcam_ui_action_type_t)0;
        pthread_mutex_unlock(&ctx->action_mtx);
        if (!feedback[0])
            snprintf(feedback, sizeof(feedback), "%s",
                     rc == 0 ? "操作已完成" : "操作失败，请查看日志");
        lvgl_set_feedback(ctx, feedback, rc == 0 ? IPCAM_UI_MESSAGE_SUCCESS :
                          IPCAM_UI_MESSAGE_ERROR);
        if (rc != 0)
            MLOGW("UI action failed: action=%d rc=%d\n", (int)action.type, rc);
    }
}

/* touch 线程只更新数据快照，不能跨线程调用任何 LVGL API。 */
void ipcam_lvgl_touch_report(ipcam_lvgl_ctx_t *ctx,
                             const ipcam_touch_point_t *points, int count)
{
    if (!ctx || !points || count < 0) return;
    pthread_mutex_lock(&ctx->input_mtx);
    if (count > 0) {
        ctx->touch_pressed = 1;
        ctx->touch_x = points[0].x;
        ctx->touch_y = points[0].y;
    } else {
        ctx->touch_pressed = 0;
    }
    pthread_mutex_unlock(&ctx->input_mtx);
}

/* 复制 LVGL 固定窗口快照；排序 P95 只处理最多 64 个样本，不触发堆分配。 */
void ipcam_lvgl_get_perf(ipcam_lvgl_ctx_t *ctx, ipcam_lvgl_perf_t *out)
{
    if (!ctx || !out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&ctx->stats_mtx);
    out->handler_avg_ns = ipcam_perf_window_avg(&ctx->handler_window);
    out->handler_p95_ns = ipcam_perf_window_p95(&ctx->handler_window);
    out->handler_max_ns = ipcam_perf_window_max(&ctx->handler_window);
    out->video_avg_ns = ipcam_perf_window_avg(&ctx->video_window);
    out->video_p95_ns = ipcam_perf_window_p95(&ctx->video_window);
    out->video_max_ns = ipcam_perf_window_max(&ctx->video_window);
    out->sleep_avg_ns = ipcam_perf_window_avg(&ctx->sleep_window);
    out->sleep_p95_ns = ipcam_perf_window_p95(&ctx->sleep_window);
    out->sleep_max_ns = ipcam_perf_window_max(&ctx->sleep_window);
    out->handler_count = ctx->handler_count;
    out->video_frames = ctx->video_frames;
    out->sleep_redraws = ctx->sleep_prompt_redraws;
    out->sleep_fast_path = ctx->sleep_fast_path;
    out->sleep_fast_active = ctx->sleep_fast_active;
    pthread_mutex_unlock(&ctx->stats_mtx);
}

/* 触摸必须先停，保证 LVGL 线程退出后不会再有输入快照写入 mutex。 */
void ipcam_lvgl_stop(ipcam_lvgl_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->service_running = 0;
    if (ctx->thread_started) {
        pthread_join(ctx->thread, NULL);
        ctx->thread_started = 0;
    }
    if (ctx->ui) {
        ipcam_ui_destroy(ctx->ui);
        ctx->ui = NULL;
    }
    if (lv_is_initialized()) lv_deinit();
    ctx->lv_display = NULL;
    ctx->lv_indev = NULL;
    free(ctx->video_buf);
    free(ctx->draw_buf);
    free(ctx->sleep_lut);
    free(ctx->sleep_video_buf);
    free(ctx->sleep_bg_buf);
    ctx->video_buf = NULL;
    ctx->draw_buf = NULL;
    ctx->sleep_lut = NULL;
    ctx->sleep_video_buf = NULL;
    ctx->sleep_bg_buf = NULL;
    pthread_mutex_destroy(&ctx->service_mtx);
    pthread_mutex_destroy(&ctx->feedback_mtx);
    pthread_mutex_destroy(&ctx->action_mtx);
    pthread_mutex_destroy(&ctx->input_mtx);
    pthread_mutex_destroy(&ctx->stats_mtx);
    memset(ctx, 0, sizeof(*ctx));
}
