#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Fixed-capacity ring of formatted log lines, drop-oldest on overflow.
//
// Pure logic with no ESP-IDF/FreeRTOS dependencies — host-tested in
// test/log_ring/ (same pattern as test/ble_decoders). NOT thread-safe by
// itself: logging.cpp wraps every call in its format mutex (see the
// threading contract there). Drop-OLDEST is deliberate: when debugging,
// the most recent lines are the valuable ones.
class LogRing {
public:
    static constexpr size_t MAX_LINE = 224;  // stored bytes per line incl NUL; longer input truncates
    static constexpr size_t CAPACITY = 12;   // lines buffered between drains (~2.7 KB)

    // Copy `line` in (need not be NUL-terminated; `len` governs). When the
    // ring is full the oldest line is evicted and dropped() increments.
    void push(const char *line, size_t len) {
        if (len >= MAX_LINE) len = MAX_LINE - 1;
        if (_count == CAPACITY) {
            _tail = (_tail + 1) % CAPACITY;
            _count--;
            _dropped++;
        }
        Slot &s = _slot[(_tail + _count) % CAPACITY];
        memcpy(s.text, line, len);
        s.text[len] = '\0';
        s.len = (uint16_t)len;
        _count++;
    }

    // Pop the oldest line into `out` (cap counts the NUL). Returns the
    // copied length, or 0 when the ring is empty or cap is 0.
    size_t pop(char *out, size_t cap) {
        if (_count == 0 || cap == 0) return 0;
        const Slot &s = _slot[_tail];
        size_t len = s.len;
        if (len >= cap) len = cap - 1;
        memcpy(out, s.text, len);
        out[len] = '\0';
        _tail = (_tail + 1) % CAPACITY;
        _count--;
        return len;
    }

    size_t   count() const   { return _count; }
    uint32_t dropped() const { return _dropped; }  // cumulative evictions

private:
    struct Slot { uint16_t len; char text[MAX_LINE]; };
    Slot     _slot[CAPACITY] = {};
    size_t   _tail = 0;   // index of the oldest line
    size_t   _count = 0;
    uint32_t _dropped = 0;
};
