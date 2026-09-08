#include "ipcam_light.h"

#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define IPCAM_LIGHT_LOG_MODULE "LIGHT"

/*
 * 自动点灯阈值使用 8x8 个 Y 样本的平均值。开启阈值和关闭阈值刻意分开，
 * 再配合连续帧计数，避免 OV5640 自动曝光变化时白光在临界亮度反复闪烁。
 */
#define IPCAM_LIGHT_ON_Y          35U
#define IPCAM_LIGHT_OFF_Y         55U
#define IPCAM_LIGHT_ON_FRAMES     15U
#define IPCAM_LIGHT_OFF_FRAMES    30U
#define IPCAM_LIGHT_MIN_HOLD_US   3000000ULL

/* 旧版交叉工具链的 UAPI 头可能没有 flash 控件宏，按内核 UAPI 值补齐。 */
#ifndef V4L2_CTRL_CLASS_FLASH
#define V4L2_CTRL_CLASS_FLASH 0x00a30000
#endif
#ifndef V4L2_CID_FLASH_CLASS_BASE
#define V4L2_CID_FLASH_CLASS_BASE (V4L2_CTRL_CLASS_FLASH | 0x900)
#endif
#ifndef V4L2_CID_FLASH_LED_MODE
#define V4L2_CID_FLASH_LED_MODE (V4L2_CID_FLASH_CLASS_BASE + 1)
#endif
#ifndef V4L2_FLASH_LED_MODE_NONE
#define V4L2_FLASH_LED_MODE_NONE  0
#define V4L2_FLASH_LED_MODE_FLASH 1
#define V4L2_FLASH_LED_MODE_TORCH 2
#endif

static int light_text_matches(const char *text)
{
    char normalized[64];
    size_t out = 0;

    if (!text) return 0;
    for (size_t i = 0; text[i] && out + 1 < sizeof(normalized); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            normalized[out++] = (char)c;
    }
    normalized[out] = '\0';
    return strstr(normalized, "camera") != NULL ||
           strstr(normalized, "flash") != NULL ||
           strstr(normalized, "white") != NULL;
}

static int light_safe_sysfs_path(const char *path)
{
    const char *prefix = "/sys/class/leds/";
    const char *suffix = "/brightness";
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    size_t len;
    size_t name_len;
    const char *name;
    char led_name[64];

    if (!path || strncmp(path, prefix, prefix_len) != 0) return 0;
    len = strlen(path);
    if (len <= prefix_len + suffix_len ||
        strcmp(path + len - suffix_len, suffix) != 0)
        return 0;

    name = path + prefix_len;
    name_len = (size_t)(path + len - suffix_len - name);
    if (name_len == 0 || name_len >= sizeof(led_name) ||
        memchr(name, '/', name_len) != NULL)
        return 0;
    memcpy(led_name, name, name_len);
    led_name[name_len] = '\0';
    /* 明确拒绝状态灯，避免“白光不可用”时误点亮开发板 sys-led。 */
    if (strcmp(led_name, "sys-led") == 0 || strcmp(led_name, "led0") == 0)
        return 0;
    return light_text_matches(led_name);
}

static int light_find_sysfs_endpoint(char *path, size_t path_size)
{
    const char *override = getenv("IPCAM_WHITE_LIGHT_SYSFS");
    glob_t matches;
    size_t valid = 0;
    int result = -1;

    if (override && *override) {
        if (!light_safe_sysfs_path(override)) {
            MLOGW_M(IPCAM_LIGHT_LOG_MODULE,
                    "reject unsafe white-light sysfs path: %s\n", override);
            return -1;
        }
        if (strlen(override) >= path_size) return -1;
        strcpy(path, override);
        return 0;
    }

    memset(&matches, 0, sizeof(matches));
    if (glob("/sys/class/leds/*/brightness", 0, NULL, &matches) != 0) {
        globfree(&matches);
        return -1;
    }
    for (size_t i = 0; i < matches.gl_pathc; i++) {
        if (!light_safe_sysfs_path(matches.gl_pathv[i])) continue;
        valid++;
        if (valid == 1 && strlen(matches.gl_pathv[i]) < path_size) {
            strcpy(path, matches.gl_pathv[i]);
            result = 0;
        }
        MLOGI_M(IPCAM_LIGHT_LOG_MODULE,
                "white-light LED candidate: %s\n", matches.gl_pathv[i]);
    }
    if (valid > 1) {
        MLOGW_M(IPCAM_LIGHT_LOG_MODULE,
                "multiple white-light LED candidates found; set "
                "IPCAM_WHITE_LIGHT_SYSFS explicitly\n");
        result = -1;
    }
    globfree(&matches);
    return result;
}

static int light_query_v4l2(int fd)
{
    struct v4l2_queryctrl query;

    if (fd < 0) return 0;
    memset(&query, 0, sizeof(query));
    query.id = V4L2_CID_FLASH_LED_MODE;
    if (ioctl(fd, VIDIOC_QUERYCTRL, &query) < 0) return 0;
    if (query.flags & V4L2_CTRL_FLAG_DISABLED) return 0;
    if (query.type != V4L2_CTRL_TYPE_MENU &&
        query.type != V4L2_CTRL_TYPE_INTEGER) return 0;
    return query.minimum <= V4L2_FLASH_LED_MODE_NONE &&
           query.maximum >= V4L2_FLASH_LED_MODE_TORCH;
}

static int light_write_state(ipcam_light_ctx_t *ctx, int on)
{
    if (!ctx) return -1;
    if (ctx->backend == IPCAM_LIGHT_BACKEND_V4L2) {
        struct v4l2_control control;
        memset(&control, 0, sizeof(control));
        control.id = V4L2_CID_FLASH_LED_MODE;
        control.value = on ? V4L2_FLASH_LED_MODE_TORCH
                           : V4L2_FLASH_LED_MODE_NONE;
        if (ioctl(ctx->video_fd, VIDIOC_S_CTRL, &control) < 0) {
            ctx->set_errors++;
            MLOGW_M(IPCAM_LIGHT_LOG_MODULE,
                    "V4L2 flash %s failed: %s\n",
                    on ? "TORCH" : "NONE", strerror(errno));
            return -1;
        }
        return 0;
    }
    if (ctx->backend == IPCAM_LIGHT_BACKEND_SYSFS && ctx->sysfs_fd >= 0) {
        const char value[2] = { on ? '1' : '0', '\n' };
        if (lseek(ctx->sysfs_fd, 0, SEEK_SET) < 0 ||
            write(ctx->sysfs_fd, value, sizeof(value)) != (ssize_t)sizeof(value)) {
            ctx->set_errors++;
            MLOGW_M(IPCAM_LIGHT_LOG_MODULE,
                    "LED class write %s failed (%s): %s\n",
                    on ? "on" : "off", ctx->endpoint, strerror(errno));
            return -1;
        }
        return 0;
    }
    return -1;
}

static void light_ensure_off(ipcam_light_ctx_t *ctx)
{
    if (!ctx || ctx->backend == IPCAM_LIGHT_BACKEND_NONE) return;

    /*
     * 新进程不能假设上一个进程已经正常退出；启动时先发一次安全的 OFF，
     * 防止旧实例或手工测试留下常亮状态。该操作只使用已经确认的后端。
     */
    if (light_write_state(ctx, 0) != 0) {
        MLOGW_M(IPCAM_LIGHT_LOG_MODULE,
                "white light backend found but initial OFF failed; auto control will retry\n");
    }
}

static int light_hold_elapsed(const ipcam_light_ctx_t *ctx, uint64_t now_us)
{
    return !ctx || ctx->last_transition_us == 0 ||
           now_us < ctx->last_transition_us ||
           now_us - ctx->last_transition_us >= IPCAM_LIGHT_MIN_HOLD_US;
}

static void light_transition(ipcam_light_ctx_t *ctx, int on,
                             unsigned int average_y, uint64_t now_us)
{
    if (!ctx || ctx->state_on == on) return;
    if (light_write_state(ctx, on) != 0) {
        /* 失败也记录重试时间，避免控制接口异常时每帧刷屏并反复发 ioctl。 */
        ctx->last_transition_us = now_us;
        ctx->dark_streak = 0;
        ctx->bright_streak = 0;
        return;
    }

    ctx->state_on = on;
    ctx->last_transition_us = now_us;
    ctx->dark_streak = 0;
    ctx->bright_streak = 0;
    MLOGI_M(IPCAM_LIGHT_LOG_MODULE,
            "white light transition: %s -> %s average_y=%u endpoint=%s\n",
            on ? "OFF" : "ON", on ? "ON" : "OFF", average_y,
            ctx->endpoint[0] ? ctx->endpoint : "v4l2-flash");
}

int ipcam_light_init(ipcam_light_ctx_t *ctx, int video_fd)
{
    const char *mode;
    char endpoint[IPCAM_LIGHT_ENDPOINT_MAX] = { 0 };

    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->video_fd = video_fd;
    ctx->sysfs_fd = -1;
    ctx->backend = IPCAM_LIGHT_BACKEND_NONE;
    ctx->auto_enabled = 1;

    mode = getenv("IPCAM_WHITE_LIGHT");
    if (mode && *mode && strcasecmp(mode, "off") == 0) {
        ctx->auto_enabled = 0;
        MLOGI_M(IPCAM_LIGHT_LOG_MODULE,
                "white light auto control disabled by IPCAM_WHITE_LIGHT=off\n");
    } else if (mode && *mode && strcasecmp(mode, "auto") != 0) {
        MLOGW_M(IPCAM_LIGHT_LOG_MODULE,
                "unknown IPCAM_WHITE_LIGHT=%s; using auto mode\n", mode);
    }

    if (light_query_v4l2(video_fd)) {
        ctx->backend = IPCAM_LIGHT_BACKEND_V4L2;
        MLOGI_M(IPCAM_LIGHT_LOG_MODULE,
                "white light backend: V4L2 flash LED_MODE (torch)\n");
        light_ensure_off(ctx);
        return 0;
    }

    if (light_find_sysfs_endpoint(endpoint, sizeof(endpoint)) == 0) {
        ctx->sysfs_fd = open(endpoint, O_WRONLY | O_CLOEXEC);
        if (ctx->sysfs_fd >= 0) {
            ctx->backend = IPCAM_LIGHT_BACKEND_SYSFS;
            /* light_find_sysfs_endpoint 已保证长度，连同 NUL 一次复制避免截断歧义。 */
            memcpy(ctx->endpoint, endpoint, strlen(endpoint) + 1);
            MLOGI_M(IPCAM_LIGHT_LOG_MODULE,
                    "white light backend: LED class %s\n", ctx->endpoint);
            light_ensure_off(ctx);
            return 0;
        }
        MLOGW_M(IPCAM_LIGHT_LOG_MODULE,
                "open white-light LED %s failed: %s\n",
                endpoint, strerror(errno));
    }

    MLOGW_M(IPCAM_LIGHT_LOG_MODULE,
            "white light control unavailable; auto detection remains logged, "
            "lamp stays off\n");
    return 0;
}

void ipcam_light_update(ipcam_light_ctx_t *ctx, unsigned int average_y,
                        uint64_t now_us)
{
    if (!ctx || !ctx->auto_enabled ||
        ctx->backend == IPCAM_LIGHT_BACKEND_NONE)
        return;

    if (!ctx->state_on) {
        if (average_y <= IPCAM_LIGHT_ON_Y)
            ctx->dark_streak++;
        else
            ctx->dark_streak = 0;
        ctx->bright_streak = 0;
        if (ctx->dark_streak >= IPCAM_LIGHT_ON_FRAMES &&
            light_hold_elapsed(ctx, now_us))
            light_transition(ctx, 1, average_y, now_us);
    } else {
        if (average_y >= IPCAM_LIGHT_OFF_Y)
            ctx->bright_streak++;
        else
            ctx->bright_streak = 0;
        ctx->dark_streak = 0;
        if (ctx->bright_streak >= IPCAM_LIGHT_OFF_FRAMES &&
            light_hold_elapsed(ctx, now_us))
            light_transition(ctx, 0, average_y, now_us);
    }
}

void ipcam_light_force_off(ipcam_light_ctx_t *ctx)
{
    if (!ctx || ctx->backend == IPCAM_LIGHT_BACKEND_NONE) return;
    /* 即使状态机认为灯已关闭也重发一次 OFF，覆盖启动关灯失败或外部改灯的情况。 */
    if (light_write_state(ctx, 0) == 0) {
        if (ctx->state_on)
            MLOGI_M(IPCAM_LIGHT_LOG_MODULE, "white light forced off\n");
        ctx->state_on = 0;
    }
}

void ipcam_light_deinit(ipcam_light_ctx_t *ctx)
{
    if (!ctx) return;
    ipcam_light_force_off(ctx);
    if (ctx->sysfs_fd >= 0) {
        close(ctx->sysfs_fd);
        ctx->sysfs_fd = -1;
    }
    ctx->backend = IPCAM_LIGHT_BACKEND_NONE;
}

const char *ipcam_light_backend_name(const ipcam_light_ctx_t *ctx)
{
    if (!ctx) return "none";
    switch (ctx->backend) {
    case IPCAM_LIGHT_BACKEND_V4L2: return "v4l2";
    case IPCAM_LIGHT_BACKEND_SYSFS: return "sysfs";
    default: return "none";
    }
}

const char *ipcam_light_state_name(const ipcam_light_ctx_t *ctx)
{
    if (!ctx) return "unknown";
    if (!ctx->auto_enabled) return "disabled";
    if (ctx->backend == IPCAM_LIGHT_BACKEND_NONE) return "unavailable";
    return ctx->state_on ? "on" : "off";
}
