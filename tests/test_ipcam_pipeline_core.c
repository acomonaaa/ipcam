#include "ipcam_perf.h"
#include "ipcam_quality.h"
#include "ipcam_ringbuffer.h"
#include "ipcam_yuyv.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct wait_context_s {
    ipcam_ring_buffer_t *rb;
    int rc;
    uint8_t data[16];
    ipcam_frame_t frame;
} wait_context_t;

/* 用独立线程验证“新序号”条件变量确实唤醒观察者，而非靠固定 sleep 猜测。 */
static void *copy_wait_thread(void *opaque)
{
    wait_context_t *ctx = (wait_context_t *)opaque;
    ctx->rc = ipcam_ring_copy_latest_wait(ctx->rb, ctx->data, sizeof(ctx->data),
                                          &ctx->frame, 0, 1000);
    return NULL;
}

static void test_ring_event_and_close(void)
{
    ipcam_ring_buffer_t *rb = ipcam_ring_create(2, sizeof(((wait_context_t *)0)->data));
    assert(rb != NULL);

    wait_context_t waiter;
    memset(&waiter, 0, sizeof(waiter));
    waiter.rb = rb;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, copy_wait_thread, &waiter) == 0);

    const uint8_t payload[] = { 0xa1, 0xb2, 0xc3 };
    assert(ipcam_ring_try_append(rb, payload, sizeof(payload)) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(waiter.rc == 0);
    assert(waiter.frame.seqNo == 1);
    assert(waiter.frame.size == sizeof(payload));
    assert(memcmp(waiter.data, payload, sizeof(payload)) == 0);

    /* 没有新序号时必须在截止时间到达后返回 1，而不能永久占住线程。 */
    assert(ipcam_ring_copy_latest_wait(rb, waiter.data, sizeof(waiter.data),
                                       &waiter.frame, 1, 0) == 1);
    ipcam_ring_close(rb);
    assert(ipcam_ring_copy_latest_wait(rb, waiter.data, sizeof(waiter.data),
                                       &waiter.frame, 1, 100) == -1);
    ipcam_ring_destroy(rb);
}

static void test_ring_timed_fifo(void)
{
    ipcam_ring_buffer_t *rb = ipcam_ring_create(1, 8);
    assert(rb != NULL);
    ipcam_frame_t frame;

    /* 空队列超时是录像线程的正常状态，不应被误报为关闭。 */
    assert(ipcam_ring_get_timed(rb, &frame, 0) == 1);
    const uint8_t value = 0x5a;
    assert(ipcam_ring_try_append(rb, &value, 1) == 0);
    assert(ipcam_ring_get_timed(rb, &frame, 100) == 0);
    assert(frame.size == 1 && *(uint8_t *)frame.rawData == value);
    ipcam_ring_release(rb);
    ipcam_ring_close(rb);
    assert(ipcam_ring_get_timed(rb, &frame, 100) == -1);
    ipcam_ring_destroy(rb);
}

static void test_ring_borrow_protection(void)
{
    const uint8_t first[] = { 1, 2, 3, 4 };
    const uint8_t second[] = { 5, 6, 7, 8 };
    const uint8_t replacement[] = { 9, 10, 11, 12 };
    ipcam_frame_t frame;
    ipcam_ring_buffer_t *rb = ipcam_ring_create(2, sizeof(first));
    assert(rb != NULL);

    assert(ipcam_ring_try_append(rb, first, sizeof(first)) == 0);
    assert(ipcam_ring_try_append(rb, second, sizeof(second)) == 0);
    assert(ipcam_ring_get_timed(rb, &frame, 0) == 0);
    assert(memcmp(frame.rawData, first, sizeof(first)) == 0);

    /* 消费者借用首槽期间，即使 latest 生产策略也不能覆盖仍在读取的 payload。 */
    assert(ipcam_ring_try_append_latest_meta(rb, replacement, sizeof(replacement), NULL) == -1);
    assert(ipcam_ring_try_append(rb, replacement, sizeof(replacement)) == -1);
    assert(memcmp(frame.rawData, first, sizeof(first)) == 0);
    assert(ipcam_ring_get_timed(rb, &frame, 0) == -1);
    ipcam_ring_release(rb);

    /* 归还后生产者才可继续使用空槽，FIFO 顺序和 payload 均应保持完整。 */
    assert(ipcam_ring_try_append(rb, replacement, sizeof(replacement)) == 0);
    assert(ipcam_ring_get_timed(rb, &frame, 0) == 0);
    assert(memcmp(frame.rawData, second, sizeof(second)) == 0);
    ipcam_ring_release(rb);
    assert(ipcam_ring_get_timed(rb, &frame, 0) == 0);
    assert(memcmp(frame.rawData, replacement, sizeof(replacement)) == 0);
    ipcam_ring_release(rb);
    ipcam_ring_destroy(rb);
}

static void test_quality_state_machine(void)
{
    ipcam_quality_controller_t controller;
    assert(ipcam_quality_init(&controller, 60, 1) == 0);
    assert(controller.effective == 60);

    /* 连续过载只降固定档位，不能每帧递减造成质量抖动。 */
    assert(ipcam_quality_update(&controller, 1, 0, 0, 66666666ULL) == 1);
    assert(controller.effective == 55);
    assert(ipcam_quality_update(&controller, 0, 70000000ULL, 0, 66666666ULL) == 1);
    assert(controller.effective == 50);
    assert(ipcam_quality_update(&controller, 0, 0, 0, 66666666ULL) == 0);

    /* 只有五个完整稳定窗口才恢复一级，避免临界负载来回切换。 */
    /* 上一条无过载样本已经开始累计稳定窗口；先用过载样本清零，
     * 让下面的断言严格覆盖“连续五个完整窗口”的边界。 */
    assert(ipcam_quality_update(&controller, 1, 0, 0, 66666666ULL) == 0);
    assert(controller.effective == 50);
    /* 落在 75%～90% 或 1.5～2 周期的迟滞区时，不能误计为稳定窗口。 */
    assert(ipcam_quality_update(&controller, 0, 60000000ULL, 1000000ULL,
                                66666666ULL) == 0);
    assert(controller.effective == 50);
    for (int i = 0; i < 4; i++) {
        assert(ipcam_quality_update(&controller, 0, 1000000ULL, 1000000ULL,
                                    66666666ULL) == 0);
        assert(controller.effective == 50);
    }
    /* 第五个稳定窗口只恢复一级；从 55 再积累完整五窗口后才恢复到 60。 */
    assert(ipcam_quality_update(&controller, 0, 1000000ULL, 1000000ULL,
                                66666666ULL) == 1);
    assert(controller.effective == 55);
    for (int i = 0; i < 4; i++) {
        assert(ipcam_quality_update(&controller, 0, 1000000ULL, 1000000ULL,
                                    66666666ULL) == 0);
        assert(controller.effective == 55);
    }
    assert(ipcam_quality_update(&controller, 0, 1000000ULL, 1000000ULL,
                                66666666ULL) == 1);
    assert(controller.effective == 60);
    assert(ipcam_quality_reconfigure(&controller, 40, 1) == 1);
    assert(controller.levels[0] == 40 && controller.levels[1] == 35 &&
           controller.levels[2] == 30 && controller.effective == 40);
}

static void fill_yuyv(uint8_t *src, int stride)
{
    const uint8_t rows[3][8] = {
        { 1, 10, 2, 20, 3, 11, 4, 21 },
        { 11, 30, 12, 40, 13, 31, 14, 41 },
        { 21, 50, 22, 60, 23, 51, 24, 61 }
    };
    for (int y = 0; y < 3; y++) {
        memcpy(src + y * stride, rows[y], sizeof(rows[y]));
        memset(src + y * stride + sizeof(rows[y]), 0xee, (size_t)stride - sizeof(rows[y]));
    }
}

static void test_yuyv_mirror_paths(void)
{
    uint8_t src[3 * 12];
    uint8_t y[3 * 4], cb[2 * 2], cr[2 * 2];
    fill_yuyv(src, 12);

    /* 高度为奇数且 stride 含 padding，最后一行色度必须自配对。 */
    assert(ipcam_yuyv_to_yuv420(src, 4, 3, 12, y, 4, cb, 2, cr, 2, 0, 0) == 0);
    const uint8_t expected_y[] = { 1, 2, 3, 4, 11, 12, 13, 14, 21, 22, 23, 24 };
    const uint8_t expected_cb[] = { 20, 21, 50, 51 };
    const uint8_t expected_cr[] = { 30, 31, 60, 61 };
    assert(memcmp(y, expected_y, sizeof(y)) == 0);
    assert(memcmp(cb, expected_cb, sizeof(cb)) == 0);
    assert(memcmp(cr, expected_cr, sizeof(cr)) == 0);

    const uint8_t expected_h_y[] = { 4, 3, 2, 1, 14, 13, 12, 11, 24, 23, 22, 21 };
    const uint8_t expected_h_cb[] = { 21, 20, 51, 50 };
    const uint8_t expected_h_cr[] = { 31, 30, 61, 60 };
    assert(ipcam_yuyv_to_yuv420(src, 4, 3, 12, y, 4, cb, 2, cr, 2, 1, 0) == 0);
    assert(memcmp(y, expected_h_y, sizeof(y)) == 0);
    assert(memcmp(cb, expected_h_cb, sizeof(cb)) == 0);
    assert(memcmp(cr, expected_h_cr, sizeof(cr)) == 0);

    const uint8_t expected_v_y[] = { 21, 22, 23, 24, 11, 12, 13, 14, 1, 2, 3, 4 };
    const uint8_t expected_v_cb[] = { 40, 41, 10, 11 };
    const uint8_t expected_v_cr[] = { 50, 51, 20, 21 };
    assert(ipcam_yuyv_to_yuv420(src, 4, 3, 12, y, 4, cb, 2, cr, 2, 0, 1) == 0);
    assert(memcmp(y, expected_v_y, sizeof(y)) == 0);
    assert(memcmp(cb, expected_v_cb, sizeof(cb)) == 0);
    assert(memcmp(cr, expected_v_cr, sizeof(cr)) == 0);

    const uint8_t expected_hv_y[] = { 24, 23, 22, 21, 14, 13, 12, 11, 4, 3, 2, 1 };
    const uint8_t expected_hv_cb[] = { 41, 40, 11, 10 };
    const uint8_t expected_hv_cr[] = { 51, 50, 21, 20 };
    assert(ipcam_yuyv_to_yuv420(src, 4, 3, 12, y, 4, cb, 2, cr, 2, 1, 1) == 0);
    assert(memcmp(y, expected_hv_y, sizeof(y)) == 0);
    assert(memcmp(cb, expected_hv_cb, sizeof(cb)) == 0);
    assert(memcmp(cr, expected_hv_cr, sizeof(cr)) == 0);

    /* YUYV 4:2:2 不接受奇数宽度；否则最后一对会越过有效行数据。 */
    assert(ipcam_yuyv_to_yuv420(src, 3, 3, 12, y, 4, cb, 2, cr, 2, 0, 0) == -1);
}

static void test_perf_window(void)
{
    ipcam_perf_window_t window;
    ipcam_perf_window_init(&window);
    for (uint64_t i = 1; i <= 20; i++) ipcam_perf_window_add(&window, i);
    assert(ipcam_perf_window_count(&window) == 20);
    assert(ipcam_perf_window_avg(&window) == 10);
    assert(ipcam_perf_window_p95(&window) == 19);
    assert(ipcam_perf_window_max(&window) == 20);
}

int main(void)
{
    test_ring_event_and_close();
    test_ring_timed_fifo();
    test_ring_borrow_protection();
    test_quality_state_machine();
    test_yuyv_mirror_paths();
    test_perf_window();
    puts("ipcam pipeline core tests: PASS");
    return 0;
}
