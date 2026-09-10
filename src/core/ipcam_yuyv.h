#ifndef IPCAM_YUYV_H
#define IPCAM_YUYV_H

/*
 * YUYV 4:2:2 到 planar YUV420 的固定路径转换。
 * 输入允许 stride 大于 width*2，输出分别使用调用方提供的行跨度；镜像
 * 组合显式分成四个函数，避免每个像素都判断镜像方向并便于逐路径测试。
 */

#include <stddef.h>
#include <stdint.h>

/*
 * 返回 0=成功、-1=参数/跨度错误。
 * width 必须为偶数（YUYV 的 U/V 以两个水平像素为单位），chroma_stride
 * 至少为 width/2；height 为奇数时最后一行会自配对。
 */
int ipcam_yuyv_to_yuv420(const uint8_t *src, int width, int height,
                         int src_stride, uint8_t *y_plane, int y_stride,
                         uint8_t *cb_plane, int cb_stride,
                         uint8_t *cr_plane, int cr_stride,
                         int mirror_h, int mirror_v);

#endif /* IPCAM_YUYV_H */
