#define _GNU_SOURCE
/* BCF2 用源文件模块名区分业务日志；LVGL 日志不能混入通用模块。 */
#define IPCAM_LOG_MODULE "LVGL"

#include "ipcam_lvgl.h"

#include "ipcam_log.h"
#include "ipcam_param.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipcam_config.h"

/* LVGL tick 使用单调时钟；不能使用 wall clock，否则校时会使动画/超时跳变。 */
static uint32_t lvgl_tick_cb(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)ts.tv_nsec / 1000000ULL);
}

/*
 * 将 LVGL 的局部 RGB565 buffer 复制到真实 framebuffer。
 * color_p 每行按 area 的宽度紧密排列，而 fb 可能有 line_length padding，
 * 所以不能把整个区域一次 memcpy；同时显式裁剪 area，防止旋转/脏区越界。
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

    const size_t source_stride = (size_t)source_width * sizeof(uint16_t);
    const size_t source_x_offset = (size_t)(x1 - area->x1) * sizeof(uint16_t);
    for (int32_t y = y1; y <= y2; y++) {
        const uint8_t *source = color_p +
                                (size_t)(y - area->y1) * source_stride +
                                source_x_offset;
        uint8_t *target = (uint8_t *)display->fb_base +
                          (size_t)(y + display->fb_yoffset) *
                              (size_t)display->fb_line_length +
                          (size_t)(x1 + display->fb_xoffset) * sizeof(uint16_t);
        memcpy(target, source,
               (size_t)(x2 - x1 + 1) * sizeof(uint16_t));
    }

    /* 同步刷新是软件 memcpy，必须在回调返回前告诉 LVGL 本次 flush 已完成。 */
    lv_display_flush_ready(lv_display);
}

/* LVGL 只在自己的线程调用 read_cb；这里不直接读取 Linux fd。 */
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

/* 点击验证按钮只更新 GUI 文字，硬件/文件操作必须走上层控制队列。 */
static void lvgl_touch_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    ipcam_lvgl_ctx_t *ctx = lv_event_get_user_data(event);
    if (ctx && ctx->touch_label)
        lv_label_set_text(ctx->touch_label, "TOUCH OK");
}

/*
 * 创建第一阶段验证页面：背景显示最新 RGB565 预览，上层放置标题、链路状态
 * 和可点击按钮。这里故意使用 ASCII，避免在中文字库尚未裁剪进 rootfs 前把
 * “字显示不出来”误判成 LVGL 或 framebuffer 端口故障。
 */
static int lvgl_create_smoke_ui(ipcam_lvgl_ctx_t *ctx)
{
    lv_obj_t *screen = lv_screen_active();
    int width = ctx->display->out_w;
    int height = ctx->display->out_h;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    ctx->video_image = lv_image_create(screen);
    if (!ctx->video_image) return -1;
    lv_obj_set_pos(ctx->video_image, 0, 0);
    lv_obj_set_size(ctx->video_image, width, height);
    lv_image_set_inner_align(ctx->video_image, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_src(ctx->video_image, &ctx->video_dsc);

    lv_obj_t *top = lv_obj_create(screen);
    if (!top) return -1;
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, width, 54);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(top, 220, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);

    lv_obj_t *title = lv_label_create(top);
    if (!title) return -1;
    lv_label_set_text(title, "LVGL 9.5 / IPCAM");
    lv_obj_set_pos(title, 16, 8);
    lv_obj_set_size(title, width / 2, 24);
    lv_obj_set_style_text_color(title, lv_color_hex(0xc2ef4e), 0);

    ctx->video_state_label = lv_label_create(top);
    if (!ctx->video_state_label) return -1;
    lv_label_set_text(ctx->video_state_label, "FB READY");
    lv_obj_set_pos(ctx->video_state_label, width / 2, 8);
    lv_obj_set_size(ctx->video_state_label, width / 2 - 16, 24);
    lv_obj_set_style_text_color(ctx->video_state_label,
                                lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(ctx->video_state_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *button = lv_button_create(screen);
    if (!button) return -1;
    lv_obj_set_size(button, 180, 44);
    lv_obj_set_pos(button, (width - 180) / 2, height - 62);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xc2ef4e), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x3f3849), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_add_event_cb(button, lvgl_touch_button_event,
                        LV_EVENT_CLICKED, ctx);

    ctx->touch_label = lv_label_create(button);
    if (!ctx->touch_label) return -1;
    lv_label_set_text(ctx->touch_label, "TOUCH TEST");
    lv_obj_center(ctx->touch_label);
    lv_obj_set_style_text_color(ctx->touch_label, lv_color_hex(0x101418), 0);

    return 0;
}

/* 将 display 线程的最新 RGB565 副本交给 LVGL 图像对象，复制后才允许刷新。 */
static void lvgl_update_video(ipcam_lvgl_ctx_t *ctx)
{
    int preview_enabled = ipcam_param_get_preview_enabled() ? 1 : 0;
    if (preview_enabled != ctx->last_preview_enabled) {
        if (preview_enabled) lv_obj_remove_flag(ctx->video_image,
                                                LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ctx->video_image, LV_OBJ_FLAG_HIDDEN);
        ctx->last_preview_enabled = preview_enabled;
    }

    uint64_t rendered = 0;
    ipcam_display_get_stats(ctx->display, &rendered);
    if (!preview_enabled || rendered == ctx->last_rendered) return;

    ipcam_frame_t frame;
    if (ipcam_display_preview_acquire(ctx->display, ctx->video_buf,
                                      ctx->video_buf_size, &frame) != 0)
        return;
    if (frame.width != (uint16_t)ctx->display->out_w ||
        frame.height != (uint16_t)ctx->display->out_h ||
        frame.stride != (uint32_t)ctx->display->out_w * sizeof(uint16_t)) {
        MLOGW("LVGL preview frame layout mismatch: %ux%u stride=%u\n",
              frame.width, frame.height, frame.stride);
        return;
    }

    /* video_dsc.data 始终指向 video_buf；更新内容后让 LVGL 重绘 image。 */
    lv_image_set_src(ctx->video_image, &ctx->video_dsc);
    lv_obj_invalidate(ctx->video_image);
    ctx->last_rendered = rendered;
}

/* 只在 LVGL 线程更新诊断文字，避免从 touch 线程调用任何 lv_ API。 */
static void lvgl_update_touch_status(ipcam_lvgl_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->input_mtx);
    int pressed = ctx->touch_pressed;
    pthread_mutex_unlock(&ctx->input_mtx);
    if (pressed == ctx->last_touch_pressed) return;
    ctx->last_touch_pressed = pressed;
    if (ctx->video_state_label)
        lv_label_set_text(ctx->video_state_label,
                          pressed ? "FB READY / TOUCH" : "FB READY");
}

/* LVGL 主循环线程；所有对象、定时器和 flush 调用都在此串行执行。 */
static void *lvgl_thread(void *arg)
{
    ipcam_lvgl_ctx_t *ctx = arg;
    MLOGI("LVGL thread start, display=%dx%d\n",
          ctx->display->out_w, ctx->display->out_h);
    while (*ctx->running && ctx->service_running) {
        lvgl_update_video(ctx);
        lvgl_update_touch_status(ctx);
        lv_timer_handler();
        usleep(5 * 1000);
    }
    MLOGI("LVGL thread exit\n");
    return NULL;
}

/*
 * 初始化 LVGL 显示和输入。
 * display 已经完成 FBIOGET_*、mmap 和 RGB565 校验；本函数不重复打开
 * /dev/fb0，确保退出时只有 ipcam_display 负责解除映射和关闭 fd。
 */
int ipcam_lvgl_start(ipcam_lvgl_ctx_t *ctx, ipcam_display_ctx_t *display,
                     volatile sig_atomic_t *running)
{
    if (!ctx || !display || !running || !display->fb_base ||
        display->fb_bpp != 16 || display->out_w <= 0 || display->out_h <= 0 ||
        display->fb_line_length < display->out_w * (int)sizeof(uint16_t)) {
        MLOGE("invalid display context for LVGL\n");
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->display = display;
    ctx->running = running;
    ctx->service_running = 1;
    ctx->last_preview_enabled = -1;
    ctx->last_touch_pressed = -1;
    pthread_mutex_init(&ctx->input_mtx, NULL);

    size_t pixels = (size_t)display->out_w * (size_t)display->out_h;
    if (pixels > SIZE_MAX / sizeof(uint16_t)) goto fail_mutex;
    ctx->video_buf_size = pixels * sizeof(uint16_t);
    ctx->draw_buf_size = (size_t)display->out_w * IPCAM_LVGL_BUFFER_LINES *
                         sizeof(uint16_t);
    if (ctx->draw_buf_size == 0 || ctx->video_buf_size == 0) goto fail_mutex;

    ctx->draw_buf = malloc(ctx->draw_buf_size);
    ctx->video_buf = calloc(1, ctx->video_buf_size);
    if (!ctx->draw_buf || !ctx->video_buf) {
        MLOGE("alloc LVGL buffers failed: draw=%zu video=%zu\n",
              ctx->draw_buf_size, ctx->video_buf_size);
        goto fail_buffers;
    }

    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);
    ctx->lv_display = lv_display_create(display->out_w, display->out_h);
    if (!ctx->lv_display) goto fail_lvgl;
    lv_display_set_driver_data(ctx->lv_display, ctx);
    lv_display_set_buffers(ctx->lv_display, ctx->draw_buf, NULL,
                           (uint32_t)ctx->draw_buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(ctx->lv_display, lvgl_flush_cb);
    lv_display_set_default(ctx->lv_display);

    memset(&ctx->video_dsc, 0, sizeof(ctx->video_dsc));
    ctx->video_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ctx->video_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    ctx->video_dsc.header.w = (uint16_t)display->out_w;
    ctx->video_dsc.header.h = (uint16_t)display->out_h;
    ctx->video_dsc.header.stride =
        (uint16_t)(display->out_w * sizeof(uint16_t));
    ctx->video_dsc.data_size = (uint32_t)ctx->video_buf_size;
    ctx->video_dsc.data = ctx->video_buf;

    ctx->lv_indev = lv_indev_create();
    if (!ctx->lv_indev) goto fail_lvgl;
    lv_indev_set_type(ctx->lv_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ctx->lv_indev, lvgl_touch_read_cb);
    lv_indev_set_user_data(ctx->lv_indev, ctx);
    lv_indev_set_display(ctx->lv_indev, ctx->lv_display);

    if (lvgl_create_smoke_ui(ctx) != 0) goto fail_lvgl;

    if (pthread_create(&ctx->thread, NULL, lvgl_thread, ctx) != 0) {
        MLOGE("pthread_create LVGL failed: %s\n", strerror(errno));
        goto fail_lvgl;
    }
    MLOGI("LVGL ready: fb=%dx%d RGB565, draw=%zu bytes\n",
          display->out_w, display->out_h, ctx->draw_buf_size);
    return 0;

fail_lvgl:
    /* lv_deinit 会按 LVGL 自己的链表释放 display、indev 和页面对象。 */
    if (lv_is_initialized()) lv_deinit();
    ctx->lv_display = NULL;
    ctx->lv_indev = NULL;
fail_buffers:
    free(ctx->video_buf);
    free(ctx->draw_buf);
    ctx->video_buf = NULL;
    ctx->draw_buf = NULL;
fail_mutex:
    pthread_mutex_destroy(&ctx->input_mtx);
    memset(ctx, 0, sizeof(*ctx));
    return -1;
}

/* touch 线程只更新数据快照，不能跨线程触碰 LVGL 对象或触发重绘。 */
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

/* 触摸必须先停，保证 LVGL 线程退出后不会再有输入快照写入 mutex。 */
void ipcam_lvgl_stop(ipcam_lvgl_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->service_running = 0;
    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
        ctx->thread = 0;
    }
    if (lv_is_initialized()) lv_deinit();
    ctx->lv_display = NULL;
    ctx->lv_indev = NULL;
    free(ctx->video_buf);
    free(ctx->draw_buf);
    ctx->video_buf = NULL;
    ctx->draw_buf = NULL;
    pthread_mutex_destroy(&ctx->input_mtx);
    memset(ctx, 0, sizeof(*ctx));
}
