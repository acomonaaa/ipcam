#ifndef IPCAM_FRAME_DIAG_H
#define IPCAM_FRAME_DIAG_H

/*
 * 轻量帧内容诊断工具。
 *
 * 这里只做与像素格式无关的“采样指纹”：调用方传入每像素字节数和行跨度，
 * 工具从四个象限固定采样。这样可以用很小的 CPU 开销判断一帧在采集、ring
 * 拷贝或 framebuffer 写入过程中是否被截断、覆盖或发生半幅错位。
 */

#include <stddef.h>
#include <stdint.h>

#define IPCAM_FRAME_PROBE_QUADRANTS 4

typedef struct ipcam_frame_probe_s {
    uint32_t global;
    uint32_t quadrant[IPCAM_FRAME_PROBE_QUADRANTS];
} ipcam_frame_probe_t;

/*
 * 对 width*height 的 packed 像素做四象限采样。
 * stride_bytes 可以大于 width*bytes_per_pixel；data_size 用于最后的越界保护。
 * 成功返回 0，参数或内存布局不合法返回 -1。
 */
int ipcam_frame_probe_pixels(const void *data, size_t data_size,
                             int width, int height, int bytes_per_pixel,
                             size_t stride_bytes, ipcam_frame_probe_t *out);

#endif /* IPCAM_FRAME_DIAG_H */
