#include "ipcam_perf.h"

#include <string.h>

/* 清空固定容量窗口，避免运行期统计因 realloc 产生不可控抖动。 */
void ipcam_perf_window_init(ipcam_perf_window_t *window)
{
    ipcam_perf_window_reset(window);
}

void ipcam_perf_window_reset(ipcam_perf_window_t *window)
{
    if (!window) return;
    memset(window, 0, sizeof(*window));
}

/*
 * 写入样本并维护总和/最大值。
 * 环形覆盖时最大值可能来自被淘汰样本，因此重新扫描固定数组；64 个
 * 元素的成本远小于一帧编码，且不会把复杂的堆内存管理带进热路径。
 */
void ipcam_perf_window_add(ipcam_perf_window_t *window, uint64_t value)
{
    if (!window) return;

    if (window->count < IPCAM_PERF_SAMPLE_CAP) {
        window->samples[window->count++] = value;
        window->sum += value;
    } else {
        window->sum -= window->samples[window->next];
        window->samples[window->next] = value;
        window->sum += value;
        window->next = (window->next + 1U) % IPCAM_PERF_SAMPLE_CAP;
    }

    window->max = 0;
    for (size_t i = 0; i < window->count; i++) {
        if (window->samples[i] > window->max) window->max = window->samples[i];
    }
}

size_t ipcam_perf_window_count(const ipcam_perf_window_t *window)
{
    return window ? window->count : 0;
}

uint64_t ipcam_perf_window_avg(const ipcam_perf_window_t *window)
{
    if (!window || window->count == 0) return 0;
    return window->sum / (uint64_t)window->count;
}

uint64_t ipcam_perf_window_p95(const ipcam_perf_window_t *window)
{
    if (!window || window->count == 0) return 0;

    uint64_t sorted[IPCAM_PERF_SAMPLE_CAP];
    memcpy(sorted, window->samples, window->count * sizeof(sorted[0]));

    /* 插入排序适合固定的最多 64 个样本，避免引入 qsort 回调开销。 */
    for (size_t i = 1; i < window->count; i++) {
        uint64_t value = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = value;
    }

    /* 向上取整的 95% 分位，确保小窗口不会把高尾样本漏掉。 */
    size_t rank = (window->count * 95U + 99U) / 100U;
    if (rank == 0) rank = 1;
    if (rank > window->count) rank = window->count;
    return sorted[rank - 1U];
}

uint64_t ipcam_perf_window_max(const ipcam_perf_window_t *window)
{
    return window ? window->max : 0;
}
