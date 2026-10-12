#include <assert.h>
#include <stdio.h>
#include "serin_link/sl2_rxq.h"

int main(void) {
    sl2_rxq_t q;
    sl2_rxq_frame_t frame;
    const uint8_t src[6] = {2, 1, 2, 3, 4, 5}, dst[6] = {2, 5, 4, 3, 2, 1};
    uint8_t data[SL2_RXQ_FRAME + 1] = {0};
    sl2_rxq_init(&q);
    assert(!sl2_rxq_pop(&q, &frame));
    assert(!sl2_rxq_push(&q, src, dst, data, 0));
    assert(!sl2_rxq_push(&q, src, dst, data, -1));
    assert(!sl2_rxq_push(&q, src, dst, data, sizeof data));
    /* Set up before either task starts; cross u32 rollover while full. */
    q.head = q.tail = UINT32_MAX - 3;
    for (uint32_t i = 0; i < SL2_RXQ_SLOTS; ++i) {
        memset(data, (int) i, sizeof data);
        assert(sl2_rxq_push(&q, src, dst, data, SL2_RXQ_FRAME));
    }
    assert(!sl2_rxq_push(&q, src, dst, data, 1));
    assert(sl2_rxq_dropped(&q) == 1);
    for (uint32_t i = 0; i < SL2_RXQ_SLOTS; ++i) {
        assert(sl2_rxq_pop(&q, &frame));
        assert(frame.len == SL2_RXQ_FRAME);
        assert(memcmp(frame.src, src, 6) == 0 && memcmp(frame.dst, dst, 6) == 0);
        for (unsigned j = 0; j < frame.len; ++j) assert(frame.data[j] == i);
    }
    assert(!sl2_rxq_pop(&q, &frame));
    assert(sl2_rxq_push(&q, src, dst, data, 1));
    assert(sl2_rxq_pop(&q, &frame) && frame.len == 1);
    puts("RX queue bounds, full/empty and rollover: PASS");
}
