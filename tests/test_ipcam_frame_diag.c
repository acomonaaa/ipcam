#include "ipcam_frame_diag.h"
#include "ipcam_ringbuffer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int probe_equal(const ipcam_frame_probe_t *a,
                       const ipcam_frame_probe_t *b)
{
    if (a->global != b->global) return 0;
    for (int i = 0; i < IPCAM_FRAME_PROBE_QUADRANTS; i++)
        if (a->quadrant[i] != b->quadrant[i]) return 0;
    return 1;
}

static void test_probe_stride_and_mutation(void)
{
    unsigned char frame[8 * 12];
    ipcam_frame_probe_t first;
    ipcam_frame_probe_t second;

    /* 8x4 RGB565，行尾故意保留 4 字节 padding，验证 stride 不是 width*2。 */
    for (size_t i = 0; i < sizeof(frame); i++) frame[i] = (unsigned char)(i * 3U);
    assert(ipcam_frame_probe_pixels(frame, sizeof(frame), 4, 8, 2, 12, &first) == 0);
    assert(ipcam_frame_probe_pixels(frame, sizeof(frame), 4, 8, 2, 12, &second) == 0);
    assert(probe_equal(&first, &second));

    /* 左上象限的第一个采样点一定会变化，指纹必须能发现内容被覆盖。 */
    frame[0] ^= 0x5a;
    assert(ipcam_frame_probe_pixels(frame, sizeof(frame), 4, 8, 2, 12, &second) == 0);
    assert(!probe_equal(&first, &second));

    assert(ipcam_frame_probe_pixels(frame, sizeof(frame), 4, 8, 2, 7, &second) == -1);
    assert(ipcam_frame_probe_pixels(frame, 10, 4, 8, 2, 12, &second) == -1);
}

static void test_ring_metadata_roundtrip(void)
{
    unsigned char payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ipcam_frame_meta_t meta;
    ipcam_frame_t frame;
    ipcam_ring_buffer_t *rb = ipcam_ring_create(2, sizeof(payload));

    assert(rb != NULL);
    memset(&meta, 0, sizeof(meta));
    /* 元数据直接对应当前 ring 帧头，验证采集时间、几何/格式和配置代次
     * 会随 payload 一起往返；不依赖已删除的 source_* 诊断字段。 */
    meta.monotonic_ns = 123456789ULL;
    meta.width = 640;
    meta.height = 480;
    meta.stride = 1280;
    meta.pixel_format = 0x56595559U; /* V4L2_PIX_FMT_YUYV */
    meta.config_generation = 7;

    assert(ipcam_ring_try_append_meta(rb, payload, sizeof(payload), &meta) == 0);
    assert(ipcam_ring_get(rb, &frame) == 0);
    assert(frame.seqNo == 1);
    assert(frame.size == sizeof(payload));
    assert(memcmp(frame.rawData, payload, sizeof(payload)) == 0);
    assert(frame.monotonic_ns == meta.monotonic_ns);
    assert(frame.width == meta.width);
    assert(frame.height == meta.height);
    assert(frame.stride == meta.stride);
    assert(frame.pixel_format == meta.pixel_format);
    assert(frame.config_generation == meta.config_generation);
    ipcam_ring_release(rb);
    ipcam_ring_destroy(rb);
}

int main(void)
{
    test_probe_stride_and_mutation();
    test_ring_metadata_roundtrip();
    puts("ipcam frame diagnostic tests: PASS");
    return 0;
}
