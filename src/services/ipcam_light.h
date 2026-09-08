#ifndef IPCAM_LIGHT_H
#define IPCAM_LIGHT_H

#include <stdint.h>

#define IPCAM_LIGHT_ENDPOINT_MAX 192

typedef enum ipcam_light_backend_e {
    IPCAM_LIGHT_BACKEND_NONE = 0,
    IPCAM_LIGHT_BACKEND_V4L2,
    IPCAM_LIGHT_BACKEND_SYSFS
} ipcam_light_backend_t;

typedef struct ipcam_light_ctx_s {
    int video_fd;                    /* 借用 capture 的摄像头 fd，不在此处关闭 */
    int sysfs_fd;                    /* 可选 LED class brightness fd */
    int auto_enabled;
    int state_on;
    ipcam_light_backend_t backend;
    char endpoint[IPCAM_LIGHT_ENDPOINT_MAX];
    unsigned int dark_streak;
    unsigned int bright_streak;
    uint64_t last_transition_us;      /* 上次成功切换或失败重试时间 */
    unsigned long set_errors;
} ipcam_light_ctx_t;

/* 初始化控制后端；未发现安全后端时返回 0，但状态保持不可用。 */
int ipcam_light_init(ipcam_light_ctx_t *ctx, int video_fd);

/* 用一帧的平均亮度驱动带滞回和去抖的自动点灯状态机。 */
void ipcam_light_update(ipcam_light_ctx_t *ctx, unsigned int average_y,
                        uint64_t now_us);

/* 退出采集前强制关灯，避免应用异常停止后白光持续点亮。 */
void ipcam_light_force_off(ipcam_light_ctx_t *ctx);

/* 释放可选的 LED class fd，并保证灯关闭。 */
void ipcam_light_deinit(ipcam_light_ctx_t *ctx);

const char *ipcam_light_backend_name(const ipcam_light_ctx_t *ctx);
const char *ipcam_light_state_name(const ipcam_light_ctx_t *ctx);

#endif /* IPCAM_LIGHT_H */
