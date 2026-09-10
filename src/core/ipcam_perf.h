#ifndef IPCAM_PERF_H
#define IPCAM_PERF_H

/*
 * 固定容量的媒体时延统计基础设施。
 *
 * 该模块只保存最近 IPCAM_PERF_SAMPLE_CAP 个样本，不在帧热路径上申请
 * 内存；调用方负责用同一把业务锁保护一个窗口，或保证只有一个写线程。
 * p95 是窗口内的离散近似值，适合板端每秒汇总和质量控制。
 */

#include <stddef.h>
#include <stdint.h>

#define IPCAM_PERF_SAMPLE_CAP 64U

typedef struct ipcam_perf_window_s {
    uint64_t samples[IPCAM_PERF_SAMPLE_CAP];
    size_t count;
    size_t next;
    uint64_t sum;
    uint64_t max;
} ipcam_perf_window_t;

/* 初始化或清空统计窗口；不触碰调用方的外围锁。 */
void ipcam_perf_window_init(ipcam_perf_window_t *window);
void ipcam_perf_window_reset(ipcam_perf_window_t *window);

/* 写入一个纳秒样本；超过固定容量后按环形方式淘汰最旧样本。 */
void ipcam_perf_window_add(ipcam_perf_window_t *window, uint64_t value);

/* 返回窗口当前样本数、平均值、P95 和最大值；空窗口返回 0。 */
size_t ipcam_perf_window_count(const ipcam_perf_window_t *window);
uint64_t ipcam_perf_window_avg(const ipcam_perf_window_t *window);
uint64_t ipcam_perf_window_p95(const ipcam_perf_window_t *window);
uint64_t ipcam_perf_window_max(const ipcam_perf_window_t *window);

#endif /* IPCAM_PERF_H */
