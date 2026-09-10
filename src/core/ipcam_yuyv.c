#include "ipcam_yuyv.h"

/* 非镜像路径：两行、两像素一组同时生成 Y/Cb/Cr，奇数高度最后自配对。 */
static void convert_normal(const uint8_t *src, int w, int h, int src_stride,
                           uint8_t *y, int ys, uint8_t *cb, int cs,
                           uint8_t *cr, int crs)
{
    int pairs = w / 2;
    for (int row = 0; row < h; row += 2) {
        int row1 = row + 1 < h ? row + 1 : row;
        const uint8_t *in0 = src + (size_t)row * (size_t)src_stride;
        const uint8_t *in1 = src + (size_t)row1 * (size_t)src_stride;
        uint8_t *out_y0 = y + (size_t)row * (size_t)ys;
        uint8_t *out_y1 = y + (size_t)row1 * (size_t)ys;
        uint8_t *out_cb = cb + (size_t)(row / 2) * (size_t)cs;
        uint8_t *out_cr = cr + (size_t)(row / 2) * (size_t)crs;
        for (int p = 0; p < pairs; p++) {
            const uint8_t *a = in0 + (size_t)p * 4U;
            const uint8_t *b = in1 + (size_t)p * 4U;
            out_y0[p * 2] = a[0];
            out_y0[p * 2 + 1] = a[2];
            if (row1 != row) {
                out_y1[p * 2] = b[0];
                out_y1[p * 2 + 1] = b[2];
            }
            out_cb[p] = (uint8_t)(((unsigned)a[1] + b[1]) / 2U);
            out_cr[p] = (uint8_t)(((unsigned)a[3] + b[3]) / 2U);
        }
    }
}

/* 水平镜像路径：反向读取输入 pair 并交换 Y，仍在同一遍中生成色度。 */
static void convert_mirror_h(const uint8_t *src, int w, int h, int src_stride,
                             uint8_t *y, int ys, uint8_t *cb, int cs,
                             uint8_t *cr, int crs)
{
    int pairs = w / 2;
    for (int row = 0; row < h; row += 2) {
        int row1 = row + 1 < h ? row + 1 : row;
        const uint8_t *in0 = src + (size_t)row * (size_t)src_stride;
        const uint8_t *in1 = src + (size_t)row1 * (size_t)src_stride;
        uint8_t *out_y0 = y + (size_t)row * (size_t)ys;
        uint8_t *out_y1 = y + (size_t)row1 * (size_t)ys;
        uint8_t *out_cb = cb + (size_t)(row / 2) * (size_t)cs;
        uint8_t *out_cr = cr + (size_t)(row / 2) * (size_t)crs;
        for (int p = 0; p < pairs; p++) {
            int source_pair = pairs - 1 - p;
            const uint8_t *a = in0 + (size_t)source_pair * 4U;
            const uint8_t *b = in1 + (size_t)source_pair * 4U;
            out_y0[p * 2] = a[2];
            out_y0[p * 2 + 1] = a[0];
            if (row1 != row) {
                out_y1[p * 2] = b[2];
                out_y1[p * 2 + 1] = b[0];
            }
            out_cb[p] = (uint8_t)(((unsigned)a[1] + b[1]) / 2U);
            out_cr[p] = (uint8_t)(((unsigned)a[3] + b[3]) / 2U);
        }
    }
}

/* 垂直镜像路径：输出行从底部向上读取，并按输出相邻行生成色度。 */
static void convert_mirror_v(const uint8_t *src, int w, int h, int src_stride,
                             uint8_t *y, int ys, uint8_t *cb, int cs,
                             uint8_t *cr, int crs)
{
    int pairs = w / 2;
    for (int row = 0; row < h; row += 2) {
        int source0 = h - 1 - row;
        int source1 = h - 1 - (row + 1 < h ? row + 1 : row);
        const uint8_t *in0 = src + (size_t)source0 * (size_t)src_stride;
        const uint8_t *in1 = src + (size_t)source1 * (size_t)src_stride;
        uint8_t *out_y0 = y + (size_t)row * (size_t)ys;
        uint8_t *out_y1 = row + 1 < h ?
                          y + (size_t)(row + 1) * (size_t)ys : out_y0;
        uint8_t *out_cb = cb + (size_t)(row / 2) * (size_t)cs;
        uint8_t *out_cr = cr + (size_t)(row / 2) * (size_t)crs;
        for (int p = 0; p < pairs; p++) {
            const uint8_t *a = in0 + (size_t)p * 4U;
            const uint8_t *b = in1 + (size_t)p * 4U;
            out_y0[p * 2] = a[0];
            out_y0[p * 2 + 1] = a[2];
            if (row + 1 < h) {
                out_y1[p * 2] = b[0];
                out_y1[p * 2 + 1] = b[2];
            }
            out_cb[p] = (uint8_t)(((unsigned)a[1] + b[1]) / 2U);
            out_cr[p] = (uint8_t)(((unsigned)a[3] + b[3]) / 2U);
        }
    }
}

/* 双镜像路径：垂直定位输入行、水平反向 pair，并在一遍中完成三平面。 */
static void convert_mirror_hv(const uint8_t *src, int w, int h, int src_stride,
                              uint8_t *y, int ys, uint8_t *cb, int cs,
                              uint8_t *cr, int crs)
{
    int pairs = w / 2;
    for (int row = 0; row < h; row += 2) {
        int source0 = h - 1 - row;
        int source1 = h - 1 - (row + 1 < h ? row + 1 : row);
        const uint8_t *in0 = src + (size_t)source0 * (size_t)src_stride;
        const uint8_t *in1 = src + (size_t)source1 * (size_t)src_stride;
        uint8_t *out_y0 = y + (size_t)row * (size_t)ys;
        uint8_t *out_y1 = row + 1 < h ?
                          y + (size_t)(row + 1) * (size_t)ys : out_y0;
        uint8_t *out_cb = cb + (size_t)(row / 2) * (size_t)cs;
        uint8_t *out_cr = cr + (size_t)(row / 2) * (size_t)crs;
        for (int p = 0; p < pairs; p++) {
            int source_pair = pairs - 1 - p;
            const uint8_t *a = in0 + (size_t)source_pair * 4U;
            const uint8_t *b = in1 + (size_t)source_pair * 4U;
            out_y0[p * 2] = a[2];
            out_y0[p * 2 + 1] = a[0];
            if (row + 1 < h) {
                out_y1[p * 2] = b[2];
                out_y1[p * 2 + 1] = b[0];
            }
            out_cb[p] = (uint8_t)(((unsigned)a[1] + b[1]) / 2U);
            out_cr[p] = (uint8_t)(((unsigned)a[3] + b[3]) / 2U);
        }
    }
}

int ipcam_yuyv_to_yuv420(const uint8_t *src, int width, int height,
                         int src_stride, uint8_t *y_plane, int y_stride,
                         uint8_t *cb_plane, int cb_stride,
                         uint8_t *cr_plane, int cr_stride,
                         int mirror_h, int mirror_v)
{
    int chroma_width = (width + 1) / 2;
    if (!src || !y_plane || !cb_plane || !cr_plane || width <= 0 || height <= 0 ||
        (width & 1) != 0 ||
        src_stride < width * 2 || y_stride < width ||
        cb_stride < chroma_width || cr_stride < chroma_width) {
        return -1;
    }

    if (mirror_h && mirror_v) {
        convert_mirror_hv(src, width, height, src_stride, y_plane, y_stride,
                          cb_plane, cb_stride, cr_plane, cr_stride);
    } else if (mirror_h) {
        convert_mirror_h(src, width, height, src_stride, y_plane, y_stride,
                         cb_plane, cb_stride, cr_plane, cr_stride);
    } else if (mirror_v) {
        convert_mirror_v(src, width, height, src_stride, y_plane, y_stride,
                         cb_plane, cb_stride, cr_plane, cr_stride);
    } else {
        convert_normal(src, width, height, src_stride, y_plane, y_stride,
                       cb_plane, cb_stride, cr_plane, cr_stride);
    }
    return 0;
}
