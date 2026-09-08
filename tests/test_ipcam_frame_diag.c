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
    meta.source_sequence = 1234;
    meta.source_buffer_index = 2;
    meta.source_bytesperline = 1280;
    meta.source_frame_bytes = 614400;
    meta.source_probe_global = 0x12345678U;
    meta.source_probe_quadrant[0] = 0xaabbccddU;

    assert(ipcam_ring_try_append_meta(rb, payload, sizeof(payload), &meta) == 0);
    assert(ipcam_ring_get(rb, &frame) == 0);
    assert(frame.seqNo == 1);
    assert(frame.size == sizeof(payload));
    assert(memcmp(frame.rawData, payload, sizeof(payload)) == 0);
    assert(frame.meta.source_sequence == meta.source_sequence);
    assert(frame.meta.source_buffer_index == meta.source_buffer_index);
    assert(frame.meta.source_probe_global == meta.source_probe_global);
    assert(frame.meta.source_probe_quadrant[0] == meta.source_probe_quadrant[0]);
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
