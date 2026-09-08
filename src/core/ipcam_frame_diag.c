#include "ipcam_frame_diag.h"

#include <string.h>

/* FNV-1a 的初值；固定算法便于不同线程比较同一帧的指纹。 */
#define IPCAM_PROBE_HASH_INIT 2166136261U

static uint32_t probe_mix_byte(uint32_t hash, unsigned int value)
{
    hash ^= value & 0xffU;
    return hash * 16777619U;
}

static uint32_t probe_mix_coord(uint32_t hash, int x, int y, int quadrant)
{
    hash = probe_mix_byte(hash, (unsigned int)x);
    hash = probe_mix_byte(hash, (unsigned int)(x >> 8));
    hash = probe_mix_byte(hash, (unsigned int)y);
    hash = probe_mix_byte(hash, (unsigned int)(y >> 8));
    hash = probe_mix_byte(hash, (unsigned int)quadrant);
    return hash;
}

int ipcam_frame_probe_pixels(const void *data, size_t data_size,
                             int width, int height, int bytes_per_pixel,
                             size_t stride_bytes, ipcam_frame_probe_t *out)
{
    const unsigned char *bytes = data;

    if (!bytes || !out || width <= 0 || height <= 0 ||
        bytes_per_pixel <= 0 || stride_bytes < (size_t)width * (size_t)bytes_per_pixel)
        return -1;
    if ((size_t)height > SIZE_MAX / stride_bytes ||
        stride_bytes * (size_t)height > data_size)
        return -1;

    memset(out, 0, sizeof(*out));
    out->global = IPCAM_PROBE_HASH_INIT;

    for (int quadrant = 0; quadrant < IPCAM_FRAME_PROBE_QUADRANTS; quadrant++) {
        const int right = quadrant & 1;
        const int bottom = (quadrant >> 1) & 1;
        int x0 = right ? width / 2 : 0;
        int x1 = right ? width - 1 : width / 2 - 1;
        int y0 = bottom ? height / 2 : 0;
        int y1 = bottom ? height - 1 : height / 2 - 1;
        uint32_t hash = IPCAM_PROBE_HASH_INIT;

        /* width/height 至少为 1；小图时把空象限收敛到合法像素。 */
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        if (x0 >= width) x0 = width - 1;
        if (x1 >= width) x1 = width - 1;
        if (y0 >= height) y0 = height - 1;
        if (y1 >= height) y1 = height - 1;

        for (int sy = 0; sy < 4; sy++) {
            int y = y0 + (y1 - y0) * sy / 3;
            for (int sx = 0; sx < 4; sx++) {
                int x = x0 + (x1 - x0) * sx / 3;
                size_t offset;

                /* packed 4:2:2 的调用方会传偶数宽；这里仍把地址检查做完整。 */
                if ((size_t)x > (SIZE_MAX - (size_t)y * stride_bytes) /
                                (size_t)bytes_per_pixel)
                    return -1;
                offset = (size_t)y * stride_bytes +
                         (size_t)x * (size_t)bytes_per_pixel;
                if (offset > data_size ||
                    (size_t)bytes_per_pixel > data_size - offset)
                    return -1;

                hash = probe_mix_coord(hash, x, y, quadrant);
                for (int byte = 0; byte < bytes_per_pixel; byte++)
                    hash = probe_mix_byte(hash, bytes[offset + (size_t)byte]);
            }
        }
        out->quadrant[quadrant] = hash;
        out->global = probe_mix_byte(out->global, hash);
        out->global = probe_mix_byte(out->global, hash >> 8);
        out->global = probe_mix_byte(out->global, hash >> 16);
        out->global = probe_mix_byte(out->global, hash >> 24);
    }
    return 0;
}
