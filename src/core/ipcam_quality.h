#ifndef IPCAM_QUALITY_H
#define IPCAM_QUALITY_H

/*
 * JPEG 质量自适应的纯状态机。
 * 该模块不依赖线程、日志或 libjpeg，便于主机单测并确保业务层只负责
 * 提供每个统计窗口的观测值。质量等级始终从配置值向下走，避免运行中
 * 自行提高码率造成网络和编码器同时突发。
 */

#include <stdint.h>

typedef struct ipcam_quality_controller_s {
    uint8_t configured;
    uint8_t effective;
    uint8_t levels[3];
    uint8_t level;
    uint8_t enabled;
    uint8_t stable_windows;
} ipcam_quality_controller_t;

/* 以配置质量和开关初始化；返回 0=成功、-1=参数错误。 */
int ipcam_quality_init(ipcam_quality_controller_t *controller,
                       uint8_t configured, int enabled);

/* 配置修改或开关改变时重置等级，返回 1=有效质量改变、0=未改变。 */
int ipcam_quality_reconfigure(ipcam_quality_controller_t *controller,
                              uint8_t configured, int enabled);

/*
 * 完成一个统计窗口并更新质量。
 * overload 由“输入已陈旧 / 编码 P95 超过帧预算 90% / 端到端 P95 超过两周期”
 * 任一条件触发；恢复还要求编码 P95 低于 75%、端到端 P95 低于 1.5 周期，
 * 且连续 5 个满足条件的完整窗口才恢复一级，返回 1 表示质量发生变化。
 */
int ipcam_quality_update(ipcam_quality_controller_t *controller,
                         int input_stale, uint64_t encode_p95_ns,
                         uint64_t latency_p95_ns, uint64_t frame_budget_ns);

#endif /* IPCAM_QUALITY_H */
