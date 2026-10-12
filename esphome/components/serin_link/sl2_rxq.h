/*
 * sl2_rxq.h — single-producer/single-consumer frame ring for adapters.
 *
 * sl2_link_on_recv() must be called from the same context as sl2_link_loop().
 * Radio RX callbacks usually run elsewhere (the Wi-Fi task on ESP32), so the
 * adapter pushes raw frames here from the callback and drains in its loop.
 * SPSC with acquire/release publication in both directions: the consumer must
 * finish copying a slot before the producer can reuse it. Uses GCC/Clang
 * atomics shared by C and C++ adapters. Full frames are dropped and counted.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SL2_RXQ_SLOTS
#define SL2_RXQ_SLOTS 8            /* power of two */
#endif
#if SL2_RXQ_SLOTS < 2 || (SL2_RXQ_SLOTS & (SL2_RXQ_SLOTS - 1)) != 0
#error "SL2_RXQ_SLOTS must be a power of two and at least 2"
#endif
#define SL2_RXQ_FRAME 250

typedef struct {
    uint8_t  src[6];
    uint8_t  dst[6];
    uint8_t  len;
    uint8_t  data[SL2_RXQ_FRAME];
} sl2_rxq_frame_t;

typedef struct {
    uint32_t head;                 /* producer writes; atomic access only */
    uint32_t tail;                 /* consumer writes; atomic access only */
    uint32_t dropped;              /* producer writes; diagnostic readers */
    sl2_rxq_frame_t slot[SL2_RXQ_SLOTS];
} sl2_rxq_t;

/* Initialize only before the producer and consumer start. */
static inline void sl2_rxq_init(sl2_rxq_t *q) { memset(q, 0, sizeof *q); }

static inline uint32_t sl2_rxq_dropped(const sl2_rxq_t *q) {
    return __atomic_load_n(&q->dropped, __ATOMIC_RELAXED);
}

/* Producer (RX callback). Returns false when full (frame dropped). */
static inline bool sl2_rxq_push(sl2_rxq_t *q, const uint8_t src[6],
                                const uint8_t dst[6], const void *data, int len) {
    if (len <= 0 || len > SL2_RXQ_FRAME) return false;
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if (h - t >= SL2_RXQ_SLOTS) {
        __atomic_store_n(&q->dropped, sl2_rxq_dropped(q) + 1, __ATOMIC_RELAXED);
        return false;
    }
    sl2_rxq_frame_t *f = &q->slot[h & (SL2_RXQ_SLOTS - 1)];
    memcpy(f->src, src, 6);
    memcpy(f->dst, dst, 6);
    f->len = (uint8_t)len;
    memcpy(f->data, data, (size_t)len);
    __atomic_store_n(&q->head, h + 1, __ATOMIC_RELEASE);
    return true;
}

/* Consumer (loop). Returns false when empty. */
static inline bool sl2_rxq_pop(sl2_rxq_t *q, sl2_rxq_frame_t *out) {
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    if (t == h) return false;
    *out = q->slot[t & (SL2_RXQ_SLOTS - 1)];
    __atomic_store_n(&q->tail, t + 1, __ATOMIC_RELEASE);
    return true;
}

#ifdef __cplusplus
}
#endif
