#include "ipcam_scale.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void test_identity(void)
{
    const uint8_t src[] = { 0, 32, 64, 96, 128, 160 };
    uint8_t dst[sizeof(src)] = { 0 };
    int x0[3] = { 0 };
    int xfrac[3] = { 0 };

    /* 1:1 映射必须保持每个像素，验证 Q16 坐标不会产生半像素偏移。 */
    assert(ipcam_scale_build_xmap(3, 3, x0, xfrac) == 0);
    assert(x0[0] == 0 && xfrac[0] == 0);
    assert(x0[1] == 1 && xfrac[1] == 0);
    assert(x0[2] == 2 && xfrac[2] == 0);
    assert(ipcam_scale_plane_bilinear(src, 3, 2, dst, 3, 2,
                                      x0, xfrac) == 0);
    for (size_t i = 0; i < sizeof(src); i++) assert(dst[i] == src[i]);
}

static void test_upscale_edges(void)
{
    const uint8_t src[] = {
        0,   100,
        200, 255
    };
    uint8_t dst[16] = { 0 };
    int x0[4] = { 0 };
    int xfrac[4] = { 0 };

    /* 2x2 放大到 4x4，首尾像素必须仍落在源图边界。 */
    assert(ipcam_scale_build_xmap(2, 4, x0, xfrac) == 0);
    assert(x0[0] == 0 && xfrac[0] == 0);
    assert(x0[3] == 1 && xfrac[3] == 0);
    assert(ipcam_scale_plane_bilinear(src, 2, 2, dst, 4, 4,
                                      x0, xfrac) == 0);
    assert(dst[0] == 0);
    assert(dst[3] == 100);
    assert(dst[12] == 200);
    assert(dst[15] == 255);
    assert(dst[5] > dst[0] && dst[5] < dst[15]);
}

static void test_large_and_odd_dimensions(void)
{
    const int sw = 640, sh = 480, dw = 1280, dh = 720;
    uint8_t *src = malloc((size_t)sw * sh);
    uint8_t *dst = malloc((size_t)dw * dh);
    int *x0 = malloc(sizeof(*x0) * (size_t)dw);
    int *xfrac = malloc(sizeof(*xfrac) * (size_t)dw);
    assert(src && dst && x0 && xfrac);

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            /* 梯度图能同时暴露坐标比例错误和边界越界。 */
            src[(size_t)y * sw + x] = (uint8_t)((x + y) & 0xff);
        }
    }
    assert(ipcam_scale_build_xmap(sw, dw, x0, xfrac) == 0);
    assert(x0[0] == 0 && x0[dw - 1] == sw - 1);
    assert(ipcam_scale_plane_bilinear(src, sw, sh, dst, dw, dh,
                                      x0, xfrac) == 0);
    assert(dst[0] == src[0]);
    assert(dst[(size_t)(dh - 1) * dw + (dw - 1)] == src[(size_t)(sh - 1) * sw + (sw - 1)]);

    uint8_t odd_src[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t odd_dst[35] = { 0 };
    int odd_x0[5] = { 0 };
    int odd_xfrac[5] = { 0 };
    assert(ipcam_scale_build_xmap(3, 5, odd_x0, odd_xfrac) == 0);
    assert(ipcam_scale_plane_bilinear(odd_src, 3, 3, odd_dst, 5, 7,
                                      odd_x0, odd_xfrac) == 0);

    free(xfrac);
    free(x0);
    free(dst);
    free(src);
}

static void test_invalid_arguments(void)
{
    int x0[2] = { 0 };
    int xfrac[2] = { 0 };
    uint8_t src[4] = { 0 };
    uint8_t dst[4] = { 0 };

    assert(ipcam_scale_build_xmap(0, 2, x0, xfrac) == -1);
    assert(ipcam_scale_build_xmap(IPCAM_SCALE_MAX_DIM + 1, 2, x0, xfrac) == -1);
    assert(ipcam_scale_build_xmap(2, 2, NULL, xfrac) == -1);
    assert(ipcam_scale_plane_bilinear(src, 2, 2, dst, 0, 2,
                                      x0, xfrac) == -1);
    assert(ipcam_scale_plane_bilinear(src, IPCAM_SCALE_MAX_DIM + 1, 2,
                                      dst, 2, 2, x0, xfrac) == -1);
    assert(ipcam_scale_plane_bilinear(NULL, 2, 2, dst, 2, 2,
                                      x0, xfrac) == -1);
}

int main(void)
{
    test_identity();
    test_upscale_edges();
    test_large_and_odd_dimensions();
    test_invalid_arguments();
    puts("ipcam scale tests: PASS");
    return 0;
}
