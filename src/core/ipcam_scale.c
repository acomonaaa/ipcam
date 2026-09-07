#include "ipcam_scale.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 所有坐标都统一使用 Q16：整数 1 表示 1<<16，最后通过 >>16 还原。
 * 这里限制单边尺寸是为了让中间乘法在 int64_t 内安全完成；该上限远大于
 * 当前摄像机用途，也避免异常配置把定点计算推到未定义溢出区间。
 */
#define IPCAM_SCALE_FRAC_BITS 16
#define IPCAM_SCALE_Q16_ONE   (1LL << IPCAM_SCALE_FRAC_BITS)
#define IPCAM_SCALE_Q16_HALF  (IPCAM_SCALE_Q16_ONE >> 1)
static int scale_dims_valid(int src_w, int src_h, int dst_w, int dst_h)
{
    return src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0 &&
           src_w <= IPCAM_SCALE_MAX_DIM && src_h <= IPCAM_SCALE_MAX_DIM &&
           dst_w <= IPCAM_SCALE_MAX_DIM && dst_h <= IPCAM_SCALE_MAX_DIM;
}

int ipcam_scale_build_xmap(int src_w, int dst_w, int *x0, int *xfrac)
{
    if (src_w <= 0 || dst_w <= 0 ||
        src_w > IPCAM_SCALE_MAX_DIM || dst_w > IPCAM_SCALE_MAX_DIM ||
        !x0 || !xfrac) {
        return -1;
    }

    for (int dx = 0; dx < dst_w; dx++) {
        /*
         * 中心采样公式：((dx + 0.5) * src / dst) - 0.5。
         * 先放大到 Q16 再相减，避免浮点误差；2*dx 使用 int64_t，
         * 防止异常大尺寸在构造中间值时发生有符号整数溢出。
         */
        int64_t dst_center = (int64_t)dx * 2 + 1;
        int64_t sxq = (dst_center * src_w * IPCAM_SCALE_Q16_ONE) /
                      ((int64_t)2 * dst_w) - IPCAM_SCALE_Q16_HALF;
        int64_t max_sxq = (int64_t)(src_w - 1) * IPCAM_SCALE_Q16_ONE;

        if (sxq < 0) sxq = 0;
        if (sxq > max_sxq) sxq = max_sxq;
        x0[dx] = (int)(sxq >> IPCAM_SCALE_FRAC_BITS);
        xfrac[dx] = (int)(sxq & (IPCAM_SCALE_Q16_ONE - 1));
    }
    return 0;
}

int ipcam_scale_plane_bilinear(const uint8_t *src, int src_w, int src_h,
                               uint8_t *dst, int dst_w, int dst_h,
                               const int *x0, const int *xfrac)
{
    if (!scale_dims_valid(src_w, src_h, dst_w, dst_h) ||
        !src || !dst || !x0 || !xfrac) {
        return -1;
    }

    for (int dy = 0; dy < dst_h; dy++) {
        /* 垂直方向与横向映射使用同一套 Q16 单位，避免比例被缩小一半。 */
        int64_t dst_center = (int64_t)dy * 2 + 1;
        int64_t syq = (dst_center * src_h * IPCAM_SCALE_Q16_ONE) /
                      ((int64_t)2 * dst_h) - IPCAM_SCALE_Q16_HALF;
        int64_t max_syq = (int64_t)(src_h - 1) * IPCAM_SCALE_Q16_ONE;
        if (syq < 0) syq = 0;
        if (syq > max_syq) syq = max_syq;

        int y0 = (int)(syq >> IPCAM_SCALE_FRAC_BITS);
        int fy = (int)(syq & (IPCAM_SCALE_Q16_ONE - 1));
        int y1 = y0 + 1 < src_h ? y0 + 1 : src_h - 1;
        const uint8_t *row0 = src + (size_t)y0 * (size_t)src_w;
        const uint8_t *row1 = src + (size_t)y1 * (size_t)src_w;
        uint8_t *dst_row = dst + (size_t)dy * (size_t)dst_w;

        for (int dx = 0; dx < dst_w; dx++) {
            int left = x0[dx];
            int fx = xfrac[dx];
            /* 映射表由 build_xmap 生成；这里再次限制索引，防止外部误用。 */
            if (left < 0) left = 0;
            if (left >= src_w) left = src_w - 1;
            if (fx < 0) fx = 0;
            if (fx > (int)(IPCAM_SCALE_Q16_ONE - 1)) fx = (int)(IPCAM_SCALE_Q16_ONE - 1);

            int right = left + 1 < src_w ? left + 1 : src_w - 1;
            int p00 = row0[left], p01 = row0[right];
            int p10 = row1[left], p11 = row1[right];
            int top = p00 + (((p01 - p00) * fx) >> IPCAM_SCALE_FRAC_BITS);
            int bottom = p10 + (((p11 - p10) * fx) >> IPCAM_SCALE_FRAC_BITS);
            dst_row[dx] = (uint8_t)(top +
                (((bottom - top) * fy) >> IPCAM_SCALE_FRAC_BITS));
        }
    }
    return 0;
}
