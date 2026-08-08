/*
 * room_feed.h — the change/keepalive/stale decision shared by every
 * room-temperature source (BLE sensor, Serin Link dial).
 *
 * Pure and dependency-free so test/room_feed can prove it without a CN105, a
 * radio, or FreeRTOS. The caller supplies the clock and the reading; this
 * decides only WHETHER to write the heat pump.
 *
 * "Clear once" is the load-bearing part: a source that has stopped feeding
 * must hand the pump back to its internal thermistor exactly once, and a
 * source that was never feeding must never clear at all — that would stomp on
 * whichever source IS feeding.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define ROOM_FEED_KEEPALIVE_MS 20000u  /* resend an unchanged value this often */
#define ROOM_FEED_RESEND_MIN_MS 5000u  /* floor between change-driven resends */
#define ROOM_FEED_CHANGE_DC        5   /* 0.5 C — the CN105 wire grid */

typedef enum {
    ROOM_FEED_NONE = 0,
    ROOM_FEED_SEND,
    ROOM_FEED_CLEAR,
} room_feed_action_t;

typedef struct {
    bool     feeding;     /* we have written the pump and not yet cleared */
    bool     have_sent;   /* a value has been sent at least once */
    int16_t  last_dc;
    uint32_t last_tx_ms;
} room_feed_t;

static inline void room_feed_init(room_feed_t *f) {
    f->feeding = false; f->have_sent = false; f->last_dc = 0; f->last_tx_ms = 0;
}

static inline room_feed_action_t room_feed_step(room_feed_t *f, bool selected,
                                                bool have_reading, int16_t temp_dc,
                                                uint32_t age_ms, uint32_t now_ms,
                                                uint32_t stale_ms)
{
    const bool stale = have_reading && age_ms >= stale_ms;
    if (!selected || !have_reading || stale) {
        if (f->feeding) {          /* hand the pump back, exactly once */
            f->feeding = false; f->have_sent = false;
            return ROOM_FEED_CLEAR;
        }
        return ROOM_FEED_NONE;
    }

    const bool changed = f->have_sent && abs((int)temp_dc - (int)f->last_dc) >= ROOM_FEED_CHANGE_DC;
    const uint32_t since = now_ms - f->last_tx_ms;
    const bool due = !f->have_sent || since >= ROOM_FEED_KEEPALIVE_MS ||
                     (changed && since >= ROOM_FEED_RESEND_MIN_MS);
    if (!due) return ROOM_FEED_NONE;

    f->feeding = true; f->have_sent = true;
    f->last_dc = temp_dc; f->last_tx_ms = now_ms;
    return ROOM_FEED_SEND;
}
