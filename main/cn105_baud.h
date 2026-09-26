#pragma once
// CN105 UART baud-rate rules — no ESP-IDF deps (host-tested in
// test/cn105_baud/). Most indoor units talk at 2400; some (many SEZ-KD
// ducted units, MSZ-GC) answer only at 9600. The owner picks one in the web
// UI; nothing else is ever put on the wire.

#include <cstdint>

inline constexpr uint32_t CN105_BAUD_DEFAULT = 2400;

inline bool cn105_baud_valid(long baud) {
    return baud == 2400 || baud == 9600;
}

// The rate to run at, given what NVS returned into a 0 seed. An absent key
// (fresh device, factory reset, pre-setting firmware) leaves the 0, and it
// or any other value outside the accepted set falls back to the default
// rather than reaching the UART.
inline uint32_t cn105_baud_load(uint32_t stored) {
    return cn105_baud_valid(stored) ? stored : CN105_BAUD_DEFAULT;
}
