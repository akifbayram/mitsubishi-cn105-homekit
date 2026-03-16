#pragma once

#include "ble_config.h"

#ifdef BLE_ENABLE

#include <Arduino.h>
#include "cn105_protocol.h"

// ── Timing constants ────────────────────────────────────────────────────────
constexpr uint32_t BLE_KEEPALIVE_MS     = 20000;  // Resend temp to HP every 20s

namespace BleSensor {
    void begin();                        // Init NimBLE + start scanning
    void loop(CN105Controller &cn105);   // Keepalive + stale detection

    float    temperature();   // NAN if no data
    float    humidity();      // NAN if not supported
    int8_t   battery();       // -1 if not supported
    int      rssi();          // BLE advertisement RSSI
    bool     isActive();      // Has fresh data
    bool     isStale();       // No data for stale timeout
    uint32_t lastUpdateAge(); // ms since last reading
    bool     isEnabled();     // Feed toggle (NVS)
    void     setEnabled(bool enabled);
    void     setAddr(const char* mac);   // Update MAC, restart scan
    const char* getAddr();
}

#endif // BLE_ENABLE
