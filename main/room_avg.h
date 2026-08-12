/*
 * room_avg.h — the Average-mode room-temperature blend.
 *
 * Owns the heat pump's remote-temp register whenever roomMode == average:
 * equal-weight mean of the checked, non-stale members with per-sensor offsets
 * applied first, sent through the same room_feed change/keepalive rate
 * limiting as the single-source paths. The internal thermistor is never
 * blended with a live remote (the CN105 echoes the fed value back as the room
 * temp, so the thermistor is unobservable while feeding); internal-only
 * membership simply leaves the pump on its own sensor.
 */
#pragma once
#include <cstdint>
#include "cn105_protocol.h"

namespace RoomAvg {
    // Snapshot of the blend, recomputed once per loop() pass. Consumed by the
    // web state push, HomeKit, and the dial status mapping — carries every
    // exclusion reason so the UI renders without client-side inference.
    struct Status {
        bool     feeding;        // we own the pump's remote-temp register
        bool     fallback;       // remote members checked, none live -> internal
        float    effective;      // blended value, offsets applied; NAN unless feeding
        float    spread;         // max-min across contributors (0 if fewer than 2)
        uint32_t effAgeMs;       // freshest contributor's age; UINT32_MAX if none
        uint8_t  contributors;   // ROOM_MEMBER_* bits currently in the math
        uint8_t  exclStale;      // checked but silent past the timeout
        uint8_t  exclOff;        // checked BLE members while the master toggle is off
    };

    /* Blend + feed decision. Call ~1 Hz from the main task, after
     * BleSensor::loop() and LinkSensor::loop(). */
    void loop(CN105Controller &cn105);

    Status status();     // last loop()'s snapshot (any task)
    bool   isFeeding();  // shorthand for status().feeding

    /* Is there anything behind this member bit right now? Link needs a dial
     * that has advertised sensing hardware, a BLE bit needs its slot
     * configured, internal always exists. One rule, shared by every writer —
     * the web roomSingle/roomMembers commands, the legacy roomSource command,
     * and the dial's own source edit — so a selection can never name a source
     * that isn't there while the UI claims otherwise. */
    bool memberAvailable(int bit);

    /* The same test in the legacy sl2_room_src vocabulary the dial and the
     * pre-averaging web UI still speak. BLE is available when ANY slot is
     * configured; room_single_from_legacy() then picks which one. */
    bool legacySrcSelectable(uint8_t src);
}
