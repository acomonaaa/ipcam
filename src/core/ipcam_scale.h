#ifndef IPCAM_SCALE_H
#define IPCAM_SCALE_H

#include <stdint.h>

/*
 * 缩放模块允许的单边尺寸上限；在分配输出缓冲区前复用这个上限，
 * 防止异常配置先触发超大 malloc，之后才在定点计算阶段失败。
 */
#define IPCAM_SCALE_MAX_DIM 32768

/*
 * 为 Y 平面生成横向 Q16 坐标映射。
 * x0 保存左邻整数坐标，xfrac 保存 [0, 65535] 范围内的小数权重。
 * 返回 0 表示成功，-1 表示尺寸或输出缓冲区非法。
 */
int ipcam_scale_build_xmap(int src_w, int dst_w, int *x0, int *xfrac);

/*
 * 使用中心采样坐标做平面双线性缩放。
 * src/dst 只表示单个灰度或色度平面，调用者负责保证每行连续存储。
 * 返回值用于把非法尺寸或空指针错误传回编码线程，避免静默写越界。
 */
int ipcam_scale_plane_bilinear(const uint8_t *src, int src_w, int src_h,
                               uint8_t *dst, int dst_w, int dst_h,
                               const int *x0, const int *xfrac);

#endif /* IPCAM_SCALE_H */
