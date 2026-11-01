/*
 * link_sensor.h — Serin Link dials offering their own sensors as this unit's
 * room-temperature source.
 *
 * The dial-sourced twin of BleSensor's FEED half: no radio, no discovery, no
 * decoders — readings arrive over an already-authenticated ESP-NOW link.
 * Every bonded dial keeps its own reading, keyed by MAC (up to SL2_MAX_DIALS).
 * The shared change/keepalive/stale decision lives in room_feed.h.
 */
#pragma once
#include <cstdint>
#include "cn105_protocol.h"
#include "sl2_proto.h"
#include "sl2_bond.h"   // SL2_MAX_DIALS

namespace LinkSensor {
    /* One dial's latest report, for the Average blend and the web UI. */
    struct Reading {
        uint8_t  mac[6];
        bool     hasSensor;    /* this dial's latest SL2_DSF_HAS_SENSOR */
        float    temperature;  /* NAN until the dial sends a valid reading */
        float    humidity;     /* NAN if never reported */
        uint32_t ageMs;        /* since valid temperature; saturates at UINT32_MAX, also none */
        bool     fresh;        /* a reading younger than the stale timeout */
    };

    /* A DIAL_SENSOR arrived. Currently called from the main task — see the
     * concurrency note atop link_sensor.cpp — not a dedicated "link task". */
    void feed(const uint8_t mac[6], const struct sl2_dial_sensor_pkt *p);

    /* Drop a forgotten dial's reading (nullptr = every dial), so it stops
     * counting toward the average and the hardware gate at once instead of
     * aging out over the stale timeout. Main task. */
    void forget(const uint8_t mac[6]);

    /* Keepalive + stale detection. Call ~1 Hz from the main task, beside
     * BleSensor::loop(). */
    void loop(CN105Controller &cn105);

    /* Every dial heard from since boot (or since it was forgotten), in table
     * order. Returns the count written to out. */
    int readings(Reading out[SL2_MAX_DIALS]);

    bool  hasSensor();     /* any dial has advertised SL2_DSF_HAS_SENSOR */

    /* The SELECTED reading. When roomSourceId pins a dial (NS_LINK), that
     * dial's entry alone — the other dials read as absent, however fresh.
     * Otherwise (Average, or a non-Link source shown for reference) a summary
     * over every dial: the mean of the fresh readings, or the newest reading
     * once none is fresh, with that newest reading's age. */
    bool  isActive();      /* ...and its reading is fresh */
    bool  isStale();
    float temperature();   /* NAN if none */
    float humidity();      /* NAN if none */
    uint32_t lastUpdateAge(); /* ms since valid reading; saturates at UINT32_MAX, also none */
    uint8_t status();      /* enum sl2_room_status, for the INFO TLV */
}
