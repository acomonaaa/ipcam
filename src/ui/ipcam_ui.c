/* UI 模块需要把页面构造失败报告到与主流程相同的日志通道，便于板端串口定位。 */
#define IPCAM_LOG_MODULE "UI  "
#include "ipcam_log.h"
#include "ipcam_ui_components.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPCAM_UI_DEFAULT_WIDTH       640U
#define IPCAM_UI_DEFAULT_HEIGHT      480U
#define IPCAM_UI_DEFAULT_FPS         15U
#define IPCAM_UI_DEFAULT_JPEG        60U
#define IPCAM_UI_DEFAULT_BACKLIGHT  70U
#define IPCAM_UI_DEFAULT_TIMEOUT     3U

/* 设计稿使用二进制单位展示容量，保持状态页与首页的数值一致。 */
#define IPCAM_UI_GIB (1024ULL * 1024ULL * 1024ULL)

static uint8_t clamp_u8(int value, int min_value, int max_value)
{
    if (value < min_value) value = min_value;
    if (value > max_value) value = max_value;
    return (uint8_t)value;
}

/*
 * 先在 LVGL 线程显示“已点击/正在处理”，再等待主线程返回硬件结果。
 * 这样拍照、录像和设置命令即使需要等待文件或驱动，也不会表现成无反馈；
 * 最终成功/失败文本由 LVGL 服务用控制结果覆盖这条临时提示。
 */
static void ipcam_ui_set_local_feedback(ipcam_ui_t *ui, const char *message,
                                        ipcam_ui_message_severity_t severity)
{
    if (!ui) return;
    snprintf(ui->state.message, sizeof(ui->state.message), "%s",
             message ? message : "");
    ui->state.message_severity = severity;
    ipcam_ui_home_update(ui, &ui->local_home, 0);
    ipcam_ui_home_update(ui, &ui->computer_home, 1);
    ipcam_ui_storage_update(ui, &ui->storage);
}

/* 将控件事件交给统一分发器；此函数只做轻量数据整理，不执行硬件 I/O。 */
static void ipcam_ui_event_cb(lv_event_t *event)
{
    if (!event) return;
    ipcam_ui_action_binding_t *binding = lv_event_get_user_data(event);
    if (!binding || !binding->ui) return;
    if (lv_event_get_code(event) != binding->event_code) return;

    ipcam_ui_action_t action = binding->action;
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (action.type == IPCAM_UI_ACTION_SET_BACKLIGHT && target)
        action.value0 = lv_slider_get_value(target);
    ipcam_ui_t *ui = binding->ui;
    if (action.type == IPCAM_UI_ACTION_SET_LIGHT_TOGGLE)
        action.value0 = ui->state.light_percent ? 0 : 100;
    if (action.type == IPCAM_UI_ACTION_TOGGLE_MIRROR_HORIZONTAL) {
        int current = ui->video_pending_valid ? ui->video_pending.mirror_horizontal :
                      ui->state.mirror_horizontal;
        action.value0 = current ? 0 : 1;
    }
    if (action.type == IPCAM_UI_ACTION_TOGGLE_MIRROR_VERTICAL) {
        int current = ui->video_pending_valid ? ui->video_pending.mirror_vertical :
                      ui->state.mirror_vertical;
        action.value0 = current ? 0 : 1;
    }
    if (action.type == IPCAM_UI_ACTION_RECORD_TOGGLE) {
        action.type = ipcam_ui_record_active(ui->state.record_state) ?
                      IPCAM_UI_ACTION_RECORD_STOP : IPCAM_UI_ACTION_RECORD_START;
    }

    /* LVGL 回调所在的线程只更新 UI/动作队列，业务层稍后异步处理动作。 */
    (void)ipcam_ui_dispatch_action(ui, &action);
}

/* 将按钮动作绑定到 ui 自身的固定数组，避免事件 user_data 指向临时栈变量。 */
int ipcam_ui_bind_action(ipcam_ui_t *ui, lv_obj_t *obj,
                         lv_event_code_t event_code,
                         const ipcam_ui_action_t *action)
{
    if (!ui || !obj || !action) return -1;
    if (ui->binding_count >= IPCAM_UI_MAX_ACTION_BINDINGS) {
        MLOGE("UI action binding table full: limit=%d\n",
              IPCAM_UI_MAX_ACTION_BINDINGS);
        return -1;
    }
    ipcam_ui_action_binding_t *binding = &ui->bindings[ui->binding_count++];
    binding->ui = ui;
    binding->action = *action;
    binding->event_code = event_code;
    lv_obj_add_event_cb(obj, ipcam_ui_event_cb, event_code, binding);
    return 0;
}

/* 字库资源缺失时回退到 lv_conf.h 的默认字体，保证页面仍能创建并便于诊断。 */
const lv_font_t *ipcam_ui_font_body(const ipcam_ui_t *ui)
{
    return ui && ui->fonts.body ? ui->fonts.body : LV_FONT_DEFAULT;
}

const lv_font_t *ipcam_ui_font_small(const ipcam_ui_t *ui)
{
    return ui && ui->fonts.small ? ui->fonts.small : LV_FONT_DEFAULT;
}

const lv_font_t *ipcam_ui_font_title(const ipcam_ui_t *ui)
{
    return ui && ui->fonts.title ? ui->fonts.title : LV_FONT_DEFAULT;
}

const lv_font_t *ipcam_ui_font_icon(const ipcam_ui_t *ui)
{
    return ui && ui->fonts.icon ? ui->fonts.icon : LV_FONT_DEFAULT;
}

void ipcam_ui_state_init(ipcam_ui_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->camera_ready = 1;
    state->display_ready = 1;
    state->touch_ready = 1;
    state->network_link = 1;
    snprintf(state->network_mac, sizeof(state->network_mac),
             "--:--:--:--:--:--");
    snprintf(state->network_ip, sizeof(state->network_ip), "192.168.1.100");
    state->http_port = 8080;
    state->storage_mounted = 1;
    state->storage_total_bytes = 32ULL * IPCAM_UI_GIB;
    state->storage_available_bytes = 28ULL * IPCAM_UI_GIB + 429496730ULL;
    state->video_width = IPCAM_UI_DEFAULT_WIDTH;
    state->video_height = IPCAM_UI_DEFAULT_HEIGHT;
    state->target_fps = IPCAM_UI_DEFAULT_FPS;
    state->jpeg_quality = IPCAM_UI_DEFAULT_JPEG;
    state->measured_fps = 14.8f;
    state->preview_enabled = 1;
    state->preview_view_enabled = 1;
    state->zoom = 1.0f;
    state->backlight_percent = IPCAM_UI_DEFAULT_BACKLIGHT;
    state->screen_timeout_min = IPCAM_UI_DEFAULT_TIMEOUT;
    state->light_percent = 0;
    state->record_state = IPCAM_UI_RECORD_IDLE;
    snprintf(state->model, sizeof(state->model), "ipcam-imx6ull");
    snprintf(state->swver, sizeof(state->swver), "0.1.0");
    state->message_severity = IPCAM_UI_MESSAGE_INFO;
}

void ipcam_ui_format_bytes(uint64_t bytes, char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return;
    const char *unit = "B";
    double value = (double)bytes;
    if (bytes >= IPCAM_UI_GIB) {
        value /= (double)IPCAM_UI_GIB;
        unit = "GB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        value /= (double)(1024ULL * 1024ULL);
        unit = "MB";
    } else if (bytes >= 1024ULL) {
        value /= 1024.0;
        unit = "KB";
    }
    if (value >= 100.0 || !strcmp(unit, "B"))
        snprintf(buf, buf_sz, "%.0f %s", value, unit);
    else
        snprintf(buf, buf_sz, "%.1f %s", value, unit);
}

void ipcam_ui_format_duration(uint64_t elapsed_ms, char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return;
    uint64_t seconds = elapsed_ms / 1000ULL;
    uint64_t hours = seconds / 3600ULL;
    uint64_t minutes = (seconds % 3600ULL) / 60ULL;
    seconds %= 60ULL;
    snprintf(buf, buf_sz, "%02llu:%02llu:%02llu",
             (unsigned long long)hours, (unsigned long long)minutes,
             (unsigned long long)seconds);
}

const char *ipcam_ui_record_state_text(ipcam_ui_record_state_t state)
{
    switch (state) {
    case IPCAM_UI_RECORD_STARTING: return "准备录像";
    case IPCAM_UI_RECORDING: return "录像中";
    case IPCAM_UI_RECORD_STOPPING: return "停止录像";
    case IPCAM_UI_RECORD_ERROR: return "录像异常";
    case IPCAM_UI_RECORD_IDLE:
    default: return "未录像";
    }
}

int ipcam_ui_record_active(ipcam_ui_record_state_t state)
{
    return state == IPCAM_UI_RECORD_STARTING || state == IPCAM_UI_RECORDING ||
           state == IPCAM_UI_RECORD_STOPPING;
}

static void sync_video_pending_from_state(ipcam_ui_t *ui)
{
    ui->video_pending.width = ui->state.video_width ? ui->state.video_width :
                              IPCAM_UI_DEFAULT_WIDTH;
    ui->video_pending.height = ui->state.video_height ? ui->state.video_height :
                               IPCAM_UI_DEFAULT_HEIGHT;
    ui->video_pending.fps = ui->state.target_fps ? ui->state.target_fps :
                            IPCAM_UI_DEFAULT_FPS;
    ui->video_pending.jpeg_quality = ui->state.jpeg_quality ? ui->state.jpeg_quality :
                                     IPCAM_UI_DEFAULT_JPEG;
    ui->video_pending.mirror_horizontal = ui->state.mirror_horizontal ? 1 : 0;
    ui->video_pending.mirror_vertical = ui->state.mirror_vertical ? 1 : 0;
    ui->video_pending_valid = 1;
}

static int video_pending_matches_state(const ipcam_ui_t *ui)
{
    return ui->video_pending.width == ui->state.video_width &&
           ui->video_pending.height == ui->state.video_height &&
           ui->video_pending.fps == ui->state.target_fps &&
           ui->video_pending.jpeg_quality == ui->state.jpeg_quality &&
           ui->video_pending.mirror_horizontal == ui->state.mirror_horizontal &&
           ui->video_pending.mirror_vertical == ui->state.mirror_vertical;
}

/* 选择设置项只改变本地 pending；真正的持久化动作统一由“应用”按钮触发。 */
static void update_video_pending(ipcam_ui_t *ui, const ipcam_ui_action_t *action)
{
    if (!ui || !action) return;
    if (!ui->video_pending_valid) sync_video_pending_from_state(ui);
    switch (action->type) {
    case IPCAM_UI_ACTION_SELECT_VIDEO_PRESET:
        if (action->value0 > 0) ui->video_pending.width = (uint16_t)action->value0;
        if (action->value1 > 0) ui->video_pending.height = (uint16_t)action->value1;
        if (action->value2 > 0) ui->video_pending.fps = (uint8_t)action->value2;
        break;
    case IPCAM_UI_ACTION_SELECT_JPEG_QUALITY:
        ui->video_pending.jpeg_quality = clamp_u8(action->value0, 1, 100);
        break;
    case IPCAM_UI_ACTION_TOGGLE_MIRROR_HORIZONTAL:
        ui->video_pending.mirror_horizontal = action->value0 ? 1 : 0;
        break;
    case IPCAM_UI_ACTION_TOGGLE_MIRROR_VERTICAL:
        ui->video_pending.mirror_vertical = action->value0 ? 1 : 0;
        break;
    default:
        return;
    }
    ui->video_dirty = !video_pending_matches_state(ui);
    ipcam_ui_video_update(ui, &ui->video);
}

static int dispatch_business_action(ipcam_ui_t *ui,
                                    const ipcam_ui_action_t *action)
{
    if (!ui || !action || !ui->actions.on_action) return 0;
    return ui->actions.on_action(action, ui->actions.opaque);
}

/*
 * 事件回调只记录最后一个导航目标，不能在回调栈内删除当前页面。
 * 页面按钮可能连续产生多个触摸事件，保留最新目标即可避免无意义地创建中间页。
 */
static int ipcam_ui_request_screen(ipcam_ui_t *ui, ipcam_ui_screen_t screen)
{
    if (!ui || screen < IPCAM_UI_SCREEN_LOCAL_HOME ||
        screen > IPCAM_UI_SCREEN_STATE)
        return -1;
    ui->pending_screen = screen;
    ui->navigation_pending = 1;
    return 0;
}

/* 页面导航在 UI 层完成，设备控制动作只通过回调交给上层业务队列。 */
int ipcam_ui_dispatch_action(ipcam_ui_t *ui, const ipcam_ui_action_t *input)
{
    if (!ui || !input) return -1;
    ipcam_ui_action_t action = *input;
    switch (action.type) {
    case IPCAM_UI_ACTION_SHOW_LOCAL:
        ui->home_screen = IPCAM_UI_SCREEN_LOCAL_HOME;
        return ipcam_ui_request_screen(ui, IPCAM_UI_SCREEN_LOCAL_HOME);
    case IPCAM_UI_ACTION_SHOW_COMPUTER:
        ui->home_screen = IPCAM_UI_SCREEN_COMPUTER_HOME;
        return ipcam_ui_request_screen(ui, IPCAM_UI_SCREEN_COMPUTER_HOME);
    case IPCAM_UI_ACTION_SHOW_FULLSCREEN:
        return ipcam_ui_request_screen(ui, IPCAM_UI_SCREEN_FULLSCREEN);
    case IPCAM_UI_ACTION_OPEN_VIDEO_SETTINGS:
        return ipcam_ui_request_screen(ui, IPCAM_UI_SCREEN_VIDEO_SETTINGS);
    case IPCAM_UI_ACTION_OPEN_SCREEN_SETTINGS:
        return ipcam_ui_request_screen(ui, IPCAM_UI_SCREEN_SCREEN_SETTINGS);
    case IPCAM_UI_ACTION_OPEN_STORAGE:
        return ipcam_ui_request_screen(ui, IPCAM_UI_SCREEN_STORAGE);
    case IPCAM_UI_ACTION_OPEN_NETWORK:
        return ipcam_ui_request_screen(ui, IPCAM_UI_SCREEN_NETWORK);
    case IPCAM_UI_ACTION_OPEN_STATE:
        return ipcam_ui_request_screen(ui, IPCAM_UI_SCREEN_STATE);
    case IPCAM_UI_ACTION_BACK:
        if (ui->screen == IPCAM_UI_SCREEN_FULLSCREEN ||
            ui->screen == IPCAM_UI_SCREEN_VIDEO_SETTINGS ||
            ui->screen == IPCAM_UI_SCREEN_SCREEN_SETTINGS ||
            ui->screen == IPCAM_UI_SCREEN_STORAGE ||
            ui->screen == IPCAM_UI_SCREEN_NETWORK ||
            ui->screen == IPCAM_UI_SCREEN_STATE)
            return ipcam_ui_request_screen(ui, ui->home_screen);
        return 0;
    case IPCAM_UI_ACTION_SELECT_VIDEO_PRESET:
    case IPCAM_UI_ACTION_SELECT_JPEG_QUALITY:
        update_video_pending(ui, &action);
        ipcam_ui_set_local_feedback(ui, "视频参数已修改，请点击应用",
                                    IPCAM_UI_MESSAGE_INFO);
        return 0;
    case IPCAM_UI_ACTION_TOGGLE_MIRROR_HORIZONTAL:
    case IPCAM_UI_ACTION_TOGGLE_MIRROR_VERTICAL: {
        update_video_pending(ui, &action);
        /* 翻转是显示链路的独立运行时参数，切换后立即提交；这样即使用户
         * 没有再按“应用”，预览也会在下一帧按新的方向输出。 */
        ipcam_ui_action_t mirror = {
            .type = IPCAM_UI_ACTION_APPLY_MIRROR,
            .value0 = ui->video_pending.mirror_horizontal,
            .value1 = ui->video_pending.mirror_vertical
        };
        ipcam_ui_set_local_feedback(ui, "翻转设置正在应用",
                                    IPCAM_UI_MESSAGE_INFO);
        return dispatch_business_action(ui, &mirror);
    }
    case IPCAM_UI_ACTION_APPLY_VIDEO:
        if (!ui->video_pending_valid) sync_video_pending_from_state(ui);
        action.value0 = ui->video_pending.width;
        action.value1 = ui->video_pending.height;
        action.value2 = ui->video_pending.fps;
        action.value3 = ui->video_pending.jpeg_quality;
        action.value4 = ui->video_pending.mirror_horizontal;
        action.value5 = ui->video_pending.mirror_vertical;
        ui->video_dirty = 1;
        ipcam_ui_set_local_feedback(ui, "视频设置正在应用",
                                    IPCAM_UI_MESSAGE_INFO);
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_SET_LIGHT_TOGGLE:
        action.type = IPCAM_UI_ACTION_SET_LIGHT;
        action.value0 = clamp_u8(action.value0, 0, 100);
        ui->state.light_percent = (uint8_t)action.value0;
        ipcam_ui_set_local_feedback(ui, "补光灯设置正在应用",
                                    IPCAM_UI_MESSAGE_INFO);
        ipcam_ui_home_update(ui, &ui->local_home, 0);
        ipcam_ui_home_update(ui, &ui->computer_home, 1);
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_SET_BACKLIGHT:
        action.value0 = clamp_u8(action.value0, 10, 100);
        ui->state.backlight_percent = (uint8_t)action.value0;
        ipcam_ui_set_local_feedback(ui, "亮度设置正在应用",
                                    IPCAM_UI_MESSAGE_INFO);
        ipcam_ui_screen_settings_update(ui, &ui->screen_settings);
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_SET_SCREEN_TIMEOUT:
        action.value0 = action.value0 < 0 ? 0 : action.value0;
        ui->state.screen_timeout_min = (uint8_t)action.value0;
        ipcam_ui_set_local_feedback(ui, "自动熄屏设置正在应用",
                                    IPCAM_UI_MESSAGE_INFO);
        ipcam_ui_screen_settings_update(ui, &ui->screen_settings);
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_SCREEN_KEEP_AWAKE:
        /* 先隐藏本地提示，避免主线程处理动作前用户看到按钮无响应；真实
         * 计时器随后会把新的 last_touch 状态同步回来。 */
        ui->state.screen_sleep_prompt = 0;
        ui->state.screen_sleep_remaining_sec = 0;
        ipcam_ui_sleep_prompt_update(ui);
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_SCREEN_SLEEP:
        /* 立即休眠动作由 screen 服务执行，UI 先撤掉可点击覆盖层避免重复提交。 */
        ui->state.screen_sleep_prompt = 0;
        ui->state.screen_sleep_remaining_sec = 0;
        ipcam_ui_sleep_prompt_update(ui);
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_RECORD_TOGGLE:
        action.type = ipcam_ui_record_active(ui->state.record_state) ?
                      IPCAM_UI_ACTION_RECORD_STOP : IPCAM_UI_ACTION_RECORD_START;
        /* fall through：统一进入录像请求的即时反馈与队列提交分支。 */
    case IPCAM_UI_ACTION_RECORD_START:
    case IPCAM_UI_ACTION_RECORD_STOP:
    case IPCAM_UI_ACTION_PHOTO:
    case IPCAM_UI_ACTION_SET_PREVIEW:
    case IPCAM_UI_ACTION_RESET_VIEW:
        if (action.type == IPCAM_UI_ACTION_RECORD_START)
        {
            /* 控制请求真正由主线程执行；先显示 STARTING，防止用户在这段
             * 约 1 秒的调度窗口内再次点“开始录像”而排入重复请求。 */
            ui->state.record_state = IPCAM_UI_RECORD_STARTING;
            ipcam_ui_set_local_feedback(ui, "录像启动请求已提交",
                                        IPCAM_UI_MESSAGE_INFO);
        }
        else if (action.type == IPCAM_UI_ACTION_RECORD_STOP)
        {
            ui->state.record_state = IPCAM_UI_RECORD_STOPPING;
            ipcam_ui_set_local_feedback(ui, "录像停止请求已提交",
                                        IPCAM_UI_MESSAGE_INFO);
        }
        else if (action.type == IPCAM_UI_ACTION_PHOTO)
            ipcam_ui_set_local_feedback(ui, "正在保存照片",
                                        IPCAM_UI_MESSAGE_INFO);
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_APPLY_MIRROR:
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_STORAGE_FORMAT_REQUEST:
        ipcam_ui_storage_format_prompt(ui, 1);
        ipcam_ui_set_local_feedback(ui, "请确认格式化 SD 卡",
                                    IPCAM_UI_MESSAGE_WARNING);
        return 0;
    case IPCAM_UI_ACTION_STORAGE_FORMAT_CONFIRM:
        ipcam_ui_storage_format_prompt(ui, 0);
        ipcam_ui_set_local_feedback(ui, "正在格式化 SD 卡，请稍候",
                                    IPCAM_UI_MESSAGE_WARNING);
        return dispatch_business_action(ui, &action);
    case IPCAM_UI_ACTION_STORAGE_FORMAT_CANCEL:
        ipcam_ui_storage_format_prompt(ui, 0);
        ipcam_ui_set_local_feedback(ui, "已取消格式化",
                                    IPCAM_UI_MESSAGE_INFO);
        return 0;
    case IPCAM_UI_ACTION_SELECT_NETWORK_TAB:
        ipcam_ui_network_set_tab(&ui->network, action.value0 ? 1 : 0);
        return 0;
    default:
        return -1;
    }
}

/* 返回页面当前根对象；根对象为空表示该页面尚未按需创建或创建失败。 */
static lv_obj_t *ipcam_ui_screen_root(const ipcam_ui_t *ui,
                                      ipcam_ui_screen_t screen)
{
    if (!ui) return NULL;
    switch (screen) {
    case IPCAM_UI_SCREEN_LOCAL_HOME: return ui->local_home.root;
    case IPCAM_UI_SCREEN_COMPUTER_HOME: return ui->computer_home.root;
    case IPCAM_UI_SCREEN_FULLSCREEN: return ui->fullscreen.root;
    case IPCAM_UI_SCREEN_VIDEO_SETTINGS: return ui->video.root;
    case IPCAM_UI_SCREEN_SCREEN_SETTINGS: return ui->screen_settings.root;
    case IPCAM_UI_SCREEN_STORAGE: return ui->storage.root;
    case IPCAM_UI_SCREEN_NETWORK: return ui->network.root;
    case IPCAM_UI_SCREEN_STATE: return ui->state_view.root;
    default: return NULL;
    }
}

/* 日志使用稳定的 P01～P08 标识，避免串口环境缺少中文字库时难以检索。 */
static const char *ipcam_ui_screen_name(ipcam_ui_screen_t screen)
{
    switch (screen) {
    case IPCAM_UI_SCREEN_LOCAL_HOME: return "P01-local-home";
    case IPCAM_UI_SCREEN_COMPUTER_HOME: return "P02-computer-home";
    case IPCAM_UI_SCREEN_FULLSCREEN: return "P03-fullscreen";
    case IPCAM_UI_SCREEN_VIDEO_SETTINGS: return "P04-video-settings";
    case IPCAM_UI_SCREEN_SCREEN_SETTINGS: return "P05-screen-settings";
    case IPCAM_UI_SCREEN_STORAGE: return "P06-storage";
    case IPCAM_UI_SCREEN_NETWORK: return "P07-network";
    case IPCAM_UI_SCREEN_STATE: return "P08-state";
    default: return "unknown";
    }
}

/* 清理失败页面的局部对象，防止下一次按需创建复用悬空根指针。 */
static void ipcam_ui_reset_screen_view(ipcam_ui_t *ui, ipcam_ui_screen_t screen)
{
    lv_obj_t *root = ipcam_ui_screen_root(ui, screen);
    if (root) lv_obj_del(root);
    if (!ui) return;
    switch (screen) {
    case IPCAM_UI_SCREEN_LOCAL_HOME: memset(&ui->local_home, 0, sizeof(ui->local_home)); break;
    case IPCAM_UI_SCREEN_COMPUTER_HOME: memset(&ui->computer_home, 0, sizeof(ui->computer_home)); break;
    case IPCAM_UI_SCREEN_FULLSCREEN: memset(&ui->fullscreen, 0, sizeof(ui->fullscreen)); break;
    case IPCAM_UI_SCREEN_VIDEO_SETTINGS: memset(&ui->video, 0, sizeof(ui->video)); break;
    case IPCAM_UI_SCREEN_SCREEN_SETTINGS: memset(&ui->screen_settings, 0, sizeof(ui->screen_settings)); break;
    case IPCAM_UI_SCREEN_STORAGE: memset(&ui->storage, 0, sizeof(ui->storage)); break;
    case IPCAM_UI_SCREEN_NETWORK: memset(&ui->network, 0, sizeof(ui->network)); break;
    case IPCAM_UI_SCREEN_STATE: memset(&ui->state_view, 0, sizeof(ui->state_view)); break;
    default: break;
    }
}

/*
 * 首次访问页面时才创建它；P01 是启动必需页面，其余页面延迟到用户点击后创建。
 * 原实现一次性创建八个页面，只要任意隐藏页面的一个控件分配失败，整个 LVGL
 * 前端就会回退到视频直写，用户因此完全看不到已经可用的首页。
 */
static int ipcam_ui_ensure_screen(ipcam_ui_t *ui, ipcam_ui_screen_t screen)
{
    if (!ui || screen < IPCAM_UI_SCREEN_LOCAL_HOME ||
        screen > IPCAM_UI_SCREEN_STATE) return -1;
    if (ipcam_ui_screen_root(ui, screen)) return 0;

    /* 页面失败后会删除已创建对象；同步回退绑定计数，允许用户稍后重试而不耗尽固定表。 */
    size_t bindings_before = ui->binding_count;
    lv_obj_t *root = NULL;
    switch (screen) {
    case IPCAM_UI_SCREEN_LOCAL_HOME:
        root = ipcam_ui_home_create(ui, &ui->local_home, 0);
        break;
    case IPCAM_UI_SCREEN_COMPUTER_HOME:
        root = ipcam_ui_home_create(ui, &ui->computer_home, 1);
        break;
    case IPCAM_UI_SCREEN_FULLSCREEN:
        root = ipcam_ui_fullscreen_create(ui, &ui->fullscreen);
        break;
    case IPCAM_UI_SCREEN_VIDEO_SETTINGS:
        root = ipcam_ui_video_create(ui, &ui->video);
        break;
    case IPCAM_UI_SCREEN_SCREEN_SETTINGS:
        root = ipcam_ui_screen_settings_create(ui, &ui->screen_settings);
        break;
    case IPCAM_UI_SCREEN_STORAGE:
        root = ipcam_ui_storage_create(ui, &ui->storage);
        break;
    case IPCAM_UI_SCREEN_NETWORK:
        root = ipcam_ui_network_create(ui, &ui->network);
        break;
    case IPCAM_UI_SCREEN_STATE:
        root = ipcam_ui_state_create(ui, &ui->state_view);
        break;
    default:
        break;
    }
    if (!root) {
        MLOGE("UI page create failed: %s bindings=%zu\n",
              ipcam_ui_screen_name(screen), ui->binding_count);
        ipcam_ui_reset_screen_view(ui, screen);
        ui->binding_count = bindings_before;
        return -1;
    }
    MLOGI("UI page ready: %s\n", ipcam_ui_screen_name(screen));
    return 0;
}

/*
 * 释放所有页面根对象，只保留顶层休眠提示和共享样式/状态。
 * LVGL 页面对象数量较多，嵌入式系统反复保留 P01～P08 会造成堆碎片；
 * 每次切页重新创建一个页面可以让触控绑定表和对象生命周期保持一一对应。
 */
static void ipcam_ui_release_all_pages(ipcam_ui_t *ui)
{
    if (!ui) return;
    for (int screen = IPCAM_UI_SCREEN_LOCAL_HOME;
         screen <= IPCAM_UI_SCREEN_STATE; screen++)
        ipcam_ui_reset_screen_view(ui, (ipcam_ui_screen_t)screen);
    ui->binding_count = 0;
}

int ipcam_ui_load_screen(ipcam_ui_t *ui, ipcam_ui_screen_t screen)
{
    if (!ui || screen < IPCAM_UI_SCREEN_LOCAL_HOME ||
        screen > IPCAM_UI_SCREEN_STATE) return -1;

    /* 同一页面无需重建，尤其不能在事件回调刚结束前重复删除对象。 */
    if (ui->screen == screen && ipcam_ui_screen_root(ui, screen)) {
        lv_screen_load(ipcam_ui_screen_root(ui, screen));
        return 0;
    }

    ipcam_ui_screen_t previous_screen = ui->screen;
    int previous_exists = ipcam_ui_screen_root(ui, previous_screen) != NULL;
    ipcam_ui_release_all_pages(ui);
    if (ipcam_ui_ensure_screen(ui, screen) != 0) goto recover_previous;
    lv_obj_t *root = ipcam_ui_screen_root(ui, screen);
    if (!root) {
        MLOGE("UI page root missing after create: %s\n",
              ipcam_ui_screen_name(screen));
        goto recover_previous;
    }
    lv_screen_load(root);
    ui->screen = screen;
    return 0;

recover_previous:
    /* 目标页创建失败时尽量恢复旧页，避免用户看到空白屏幕。 */
    if (previous_exists && previous_screen != screen &&
        ipcam_ui_ensure_screen(ui, previous_screen) == 0) {
        lv_obj_t *previous_root = ipcam_ui_screen_root(ui, previous_screen);
        if (previous_root) {
            lv_screen_load(previous_root);
            ui->screen = previous_screen;
            MLOGW("UI navigation recovered previous page: %s\n",
                  ipcam_ui_screen_name(previous_screen));
        }
    }
    return -1;
}

int ipcam_ui_process_navigation(ipcam_ui_t *ui)
{
    if (!ui || !ui->navigation_pending) return 0;
    ipcam_ui_screen_t target = ui->pending_screen;
    ui->navigation_pending = 0;
    if (ipcam_ui_load_screen(ui, target) == 0) return 1;

    /* 当前页仍可用时把失败原因放在首页反馈槽，用户不会误以为触摸无效。 */
    ipcam_ui_set_local_feedback(ui, "页面打开失败，请稍后重试",
                                IPCAM_UI_MESSAGE_ERROR);
    return -1;
}

ipcam_ui_screen_t ipcam_ui_get_screen(const ipcam_ui_t *ui)
{
    return ui ? ui->screen : IPCAM_UI_SCREEN_STATE;
}

/* 返回各页面实际视频控件尺寸；非视频页返回 0×0，display 可完全停止转换。 */
int ipcam_ui_get_video_viewport(const ipcam_ui_t *ui,
                                ipcam_ui_video_viewport_t *viewport)
{
    if (!ui || !viewport) return -1;
    memset(viewport, 0, sizeof(*viewport));
    switch (ui->screen) {
    case IPCAM_UI_SCREEN_LOCAL_HOME:
    case IPCAM_UI_SCREEN_COMPUTER_HOME:
        viewport->active = 1;
        viewport->width = 476;
        viewport->height = 268;
        return 0;
    case IPCAM_UI_SCREEN_FULLSCREEN:
        viewport->active = 1;
        viewport->width = 760;
        viewport->height = 368;
        return 0;
    default:
        return 0;
    }
}

ipcam_ui_t *ipcam_ui_create(const ipcam_ui_actions_t *actions,
                            const ipcam_ui_fonts_t *fonts)
{
    ipcam_ui_t *ui = calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    if (actions) ui->actions = *actions;
    if (fonts) ui->fonts = *fonts;
    ipcam_ui_state_init(&ui->state);
    ipcam_ui_styles_init(&ui->styles, &ui->fonts);
    sync_video_pending_from_state(ui);
    ui->home_screen = IPCAM_UI_SCREEN_LOCAL_HOME;

    /* 启动只强制创建用户第一眼需要的 P01，降低嵌入式板端首次分配峰值。 */
    if (ipcam_ui_ensure_screen(ui, IPCAM_UI_SCREEN_LOCAL_HOME) != 0) {
        ipcam_ui_destroy(ui);
        return NULL;
    }
    if (ipcam_ui_load_screen(ui, IPCAM_UI_SCREEN_LOCAL_HOME) != 0) {
        ipcam_ui_destroy(ui);
        return NULL;
    }
    if (ipcam_ui_sleep_prompt_create(ui) != 0) {
        ipcam_ui_destroy(ui);
        return NULL;
    }
    MLOGI("UI manager ready: initial=P01, deferred=P02..P08\n");
    (void)ipcam_ui_update(ui, &ui->state);
    return ui;
}

void ipcam_ui_destroy(ipcam_ui_t *ui)
{
    if (!ui) return;
    if (ui->sleep_overlay) lv_obj_del(ui->sleep_overlay);
    /* 页面对象互不复用，按根对象删除即可释放全部子控件和事件回调。 */
    if (ui->local_home.root) lv_obj_del(ui->local_home.root);
    if (ui->computer_home.root) lv_obj_del(ui->computer_home.root);
    if (ui->fullscreen.root) lv_obj_del(ui->fullscreen.root);
    if (ui->video.root) lv_obj_del(ui->video.root);
    if (ui->screen_settings.root) lv_obj_del(ui->screen_settings.root);
    if (ui->storage.root) lv_obj_del(ui->storage.root);
    if (ui->network.root) lv_obj_del(ui->network.root);
    if (ui->state_view.root) lv_obj_del(ui->state_view.root);
    free(ui);
}

int ipcam_ui_update(ipcam_ui_t *ui, const ipcam_ui_state_t *state)
{
    if (!ui || !state) return -1;
    ui->state = *state;
    if (ui->state.video_width == 0) ui->state.video_width = IPCAM_UI_DEFAULT_WIDTH;
    if (ui->state.video_height == 0) ui->state.video_height = IPCAM_UI_DEFAULT_HEIGHT;
    if (ui->state.target_fps == 0) ui->state.target_fps = IPCAM_UI_DEFAULT_FPS;
    if (ui->state.jpeg_quality == 0) ui->state.jpeg_quality = IPCAM_UI_DEFAULT_JPEG;
    if (ui->state.http_port == 0) ui->state.http_port = 8080;
    if (ui->state.zoom < 1.0f) ui->state.zoom = 1.0f;
    if (ui->video_dirty && video_pending_matches_state(ui)) ui->video_dirty = 0;
    if (!ui->video_pending_valid || !ui->video_dirty) sync_video_pending_from_state(ui);

    ipcam_ui_home_update(ui, &ui->local_home, 0);
    ipcam_ui_home_update(ui, &ui->computer_home, 1);
    ipcam_ui_fullscreen_update(ui, &ui->fullscreen);
    ipcam_ui_video_update(ui, &ui->video);
    ipcam_ui_screen_settings_update(ui, &ui->screen_settings);
    ipcam_ui_storage_update(ui, &ui->storage);
    ipcam_ui_network_update(ui, &ui->network);
    ipcam_ui_state_update(ui, &ui->state_view);
    ipcam_ui_sleep_prompt_update(ui);
    return 0;
}

/* image descriptor 由 LVGL port 持有，UI 只改变源指针和占位层可见性。 */
void ipcam_ui_set_video_source(ipcam_ui_t *ui, const lv_image_dsc_t *source,
                               int valid)
{
    if (!ui) return;
    lv_obj_t *images[2] = { ui->local_home.video_image, ui->fullscreen.video_image };
    lv_obj_t *placeholders[2] = { ui->local_home.video_placeholder,
                                  ui->fullscreen.video_placeholder };
    for (size_t i = 0; i < 2; i++) {
        if (!images[i]) continue;
        if (valid && source) {
            lv_image_set_src(images[i], source);
            lv_obj_clear_flag(images[i], LV_OBJ_FLAG_HIDDEN);
            if (placeholders[i]) lv_obj_add_flag(placeholders[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(images[i], LV_OBJ_FLAG_HIDDEN);
            if (placeholders[i]) lv_obj_clear_flag(placeholders[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
