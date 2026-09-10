#include "ipcam_quality.h"

#include <limits.h>

/* 将任意配置质量限制到 libjpeg 允许的范围，保持状态机不承担参数存盘职责。 */
static uint8_t clamp_quality(uint8_t quality)
{
    if (quality < 1U) return 1U;
    if (quality > 100U) return 100U;
    return quality;
}

/* 生成 q、q-5、q-10 三个精确档位，不在这里引入浮点或平台相关舍入。 */
static void build_levels(ipcam_quality_controller_t *controller)
{
    uint8_t base = clamp_quality(controller->configured);
    controller->levels[0] = base;
    controller->levels[1] = base > 5U ? (uint8_t)(base - 5U) : 1U;
    controller->levels[2] = base > 10U ? (uint8_t)(base - 10U) : 1U;
}

int ipcam_quality_init(ipcam_quality_controller_t *controller,
                       uint8_t configured, int enabled)
{
    if (!controller || configured == 0U) return -1;
    controller->configured = clamp_quality(configured);
    controller->enabled = enabled ? 1U : 0U;
    controller->level = 0U;
    controller->stable_windows = 0U;
    build_levels(controller);
    controller->effective = controller->enabled ? controller->levels[0] :
                                                     controller->configured;
    return 0;
}

int ipcam_quality_reconfigure(ipcam_quality_controller_t *controller,
                              uint8_t configured, int enabled)
{
    if (!controller || configured == 0U) return -1;
    uint8_t old_effective = controller->effective;
    uint8_t old_enabled = controller->enabled;
    uint8_t old_configured = controller->configured;
    if (old_configured == clamp_quality(configured) && old_enabled == (enabled ? 1U : 0U)) {
        return 0;
    }
    return ipcam_quality_init(controller, configured, enabled) == 0 &&
                   old_effective != controller->effective ? 1 : 0;
}

int ipcam_quality_update(ipcam_quality_controller_t *controller,
                         int input_stale, uint64_t encode_p95_ns,
                         uint64_t latency_p95_ns, uint64_t frame_budget_ns)
{
    if (!controller || !controller->enabled || frame_budget_ns == 0) return 0;

    /* 过载和稳定使用不同的迟滞阈值：中间区间不触发降档，也不计入恢复，
     * 避免负载贴着边界抖动时在 60/55/50 之间来回切换。 */
    uint64_t encode_limit = frame_budget_ns - frame_budget_ns / 10U;
    int encode_overload = encode_p95_ns > encode_limit;
    uint64_t latency_limit = frame_budget_ns > UINT64_MAX / 2U ? UINT64_MAX :
                             frame_budget_ns * 2U;
    int latency_overload = latency_p95_ns > latency_limit;
    int overload = input_stale || encode_overload || latency_overload;
    uint64_t encode_stable_limit = frame_budget_ns - frame_budget_ns / 4U;
    uint64_t latency_stable_limit = frame_budget_ns > UINT64_MAX - frame_budget_ns / 2U ?
                                    UINT64_MAX :
                                    frame_budget_ns + frame_budget_ns / 2U;
    int stable = !input_stale &&
                 encode_p95_ns < encode_stable_limit &&
                 latency_p95_ns < latency_stable_limit;

    uint8_t old_effective = controller->effective;
    if (overload) {
        controller->stable_windows = 0U;
        if (controller->level < 2U) controller->level++;
    } else if (stable && controller->stable_windows < 5U) {
        controller->stable_windows++;
        if (controller->stable_windows >= 5U && controller->level > 0U) {
            controller->level--;
            controller->stable_windows = 0U;
        }
    } else if (!stable) {
        /* 中间迟滞区不是“健康窗口”，必须清掉已累计的恢复计数。 */
        controller->stable_windows = 0U;
    }

    controller->effective = controller->levels[controller->level];
    return old_effective != controller->effective ? 1 : 0;
}
