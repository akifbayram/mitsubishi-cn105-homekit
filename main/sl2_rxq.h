/*
 * sl2_rxq.h — single-producer/single-consumer frame ring for adapters.
 *
 * sl2_link_on_recv() must be called from the same context as sl2_link_loop().
 * Radio RX callbacks usually run elsewhere (the Wi-Fi task on ESP32), so the
 * adapter pushes raw frames here from the callback and drains in its loop.
 * SPSC with u32 indices: safe without locks on platforms where aligned u32
 * stores are atomic (ESP32-class). Frames that don't fit are dropped (counted).
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
#define SL2_RXQ_FRAME 250

typedef struct {
    uint8_t  src[6];
    uint8_t  dst[6];
    uint8_t  len;
    uint8_t  data[SL2_RXQ_FRAME];
} sl2_rxq_frame_t;

typedef struct {
    volatile uint32_t head;        /* producer writes */
    volatile uint32_t tail;        /* consumer writes */
    uint32_t dropped;              /* producer-side */
    sl2_rxq_frame_t slot[SL2_RXQ_SLOTS];
} sl2_rxq_t;

static inline void sl2_rxq_init(sl2_rxq_t *q) { memset(q, 0, sizeof *q); }

/* Producer (RX callback). Returns false when full (frame dropped). */
static inline bool sl2_rxq_push(sl2_rxq_t *q, const uint8_t src[6],
                                const uint8_t dst[6], const void *data, int len) {
    if (len <= 0 || len > SL2_RXQ_FRAME) return false;
    uint32_t h = q->head, t = q->tail;
    if (h - t >= SL2_RXQ_SLOTS) { q->dropped++; return false; }
    sl2_rxq_frame_t *f = &q->slot[h & (SL2_RXQ_SLOTS - 1)];
    memcpy(f->src, src, 6);
    memcpy(f->dst, dst, 6);
    f->len = (uint8_t)len;
    memcpy(f->data, data, (size_t)len);
    q->head = h + 1;               /* publish after the payload is in place */
    return true;
}

/* Consumer (loop). Returns false when empty. */
static inline bool sl2_rxq_pop(sl2_rxq_t *q, sl2_rxq_frame_t *out) {
    uint32_t h = q->head, t = q->tail;
    if (t == h) return false;
    *out = q->slot[t & (SL2_RXQ_SLOTS - 1)];
    q->tail = t + 1;
    return true;
}

#ifdef __cplusplus
}
#endif
