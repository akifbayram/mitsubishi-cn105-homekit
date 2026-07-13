#pragma once

#include <cstddef>
#include <cstdint>

// Fixed-capacity ring of device events, persisted as one NVS blob by
// event_log.cpp. Overwrite-oldest when full.
//
// Pure logic with no ESP-IDF dependencies — host-tested in test/event_log/
// (same pattern as test/log_ring). NOT thread-safe by itself: event_log.cpp
// wraps every call in its mutex.
//
// The blob layout is written to flash verbatim, so it must stay stable:
// grow only by claiming `reserved` fields, and bump EVENTBLOB_VERSION on any
// incompatible change (a version mismatch on load discards the old blob —
// acceptable for diagnostics history).

struct EventEntry {
    uint32_t bootN;      // boot counter when the event happened
    uint32_t uptimeS;    // seconds since that boot
    uint32_t epoch;      // unix time, 0 when the wall clock wasn't synced yet
    uint8_t  type;       // EventType (event_log.h)
    uint8_t  code;       // type-specific detail (reset reason, CN105 error code…)
    uint16_t reserved;
};
static_assert(sizeof(EventEntry) == 16, "EventEntry is persisted — keep packed/stable");

inline constexpr size_t  EVENTLOG_CAP     = 24;
inline constexpr uint8_t EVENTBLOB_VERSION = 1;

struct EventBlob {
    uint8_t ver;
    uint8_t head;        // index of the oldest entry
    uint8_t count;
    uint8_t reserved;
    EventEntry e[EVENTLOG_CAP];
};
static_assert(sizeof(EventBlob) == 4 + EVENTLOG_CAP * sizeof(EventEntry),
              "EventBlob is persisted — keep packed/stable");

inline void eventblob_reset(EventBlob &b) {
    b = EventBlob{};
    b.ver = EVENTBLOB_VERSION;
}

// A loaded blob is trusted only if this passes (wrong size is checked by the
// caller against sizeof(EventBlob)).
inline bool eventblob_valid(const EventBlob &b) {
    return b.ver == EVENTBLOB_VERSION && b.count <= EVENTLOG_CAP && b.head < EVENTLOG_CAP;
}

inline void eventblob_append(EventBlob &b, const EventEntry &ev) {
    if (b.count < EVENTLOG_CAP) {
        b.e[(b.head + b.count) % EVENTLOG_CAP] = ev;
        b.count++;
    } else {
        b.e[b.head] = ev;                       // overwrite the oldest
        b.head = (b.head + 1) % EVENTLOG_CAP;
    }
}

// Entry `i` counting back from the newest (i=0 → most recent).
// Returns nullptr past the end.
inline const EventEntry *eventblob_newest(const EventBlob &b, size_t i) {
    if (i >= b.count) return nullptr;
    return &b.e[(b.head + b.count - 1 - i) % EVENTLOG_CAP];
}
