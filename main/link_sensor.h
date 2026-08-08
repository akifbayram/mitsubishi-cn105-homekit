/*
 * link_sensor.h — a Serin Link dial offering its own sensor as this unit's
 * room-temperature source.
 *
 * The dial-sourced twin of BleSensor's FEED half: no radio, no discovery, no
 * decoders — the reading arrives over an already-authenticated ESP-NOW link.
 * The shared change/keepalive/stale decision lives in room_feed.h.
 */
#pragma once
#include <cstdint>
#include "cn105_protocol.h"
#include "sl2_proto.h"

namespace LinkSensor {
    /* A DIAL_SENSOR arrived. Currently called from the main task — see the
     * concurrency note atop link_sensor.cpp — not a dedicated "link task". */
    void feed(const uint8_t mac[6], const struct sl2_dial_sensor_pkt *p);

    /* Keepalive + stale detection. Call ~1 Hz from the main task, beside
     * BleSensor::loop(). */
    void loop(CN105Controller &cn105);

    bool  hasSensor();     /* a dial has advertised SL2_DSF_HAS_SENSOR */
    bool  isActive();      /* ...and its reading is fresh */
    bool  isStale();
    float temperature();   /* NAN if none */
    float humidity();      /* NAN if none */
    uint32_t lastUpdateAge(); /* ms since last valid reading; UINT32_MAX if none */
    uint8_t status();      /* enum sl2_room_status, for the INFO TLV */
}
