#pragma once

#ifdef BLE_SENSOR_TYPE

#include <Arduino.h>
#include "cn105_protocol.h"

// ── Sensor type constants ───────────────────────────────────────────────────
#define BLE_TYPE_GOVEE_V3  1
#define BLE_TYPE_GOVEE_V2  2
#define BLE_TYPE_PVVX      3
#define BLE_TYPE_BTHOME    4

// ── Timing constants ────────────────────────────────────────────────────────
constexpr uint32_t BLE_KEEPALIVE_MS     = 20000;  // Resend temp to HP every 20s
constexpr uint32_t BLE_STALE_TIMEOUT_MS = 90000;  // Mark stale after 90s no data

// ── Compile-time guards ─────────────────────────────────────────────────────
#if !defined(CONFIG_BT_ENABLED)
    #error "BLE_SENSOR_TYPE requires a board with Bluetooth support"
#endif

#if BLE_SENSOR_TYPE != BLE_TYPE_GOVEE_V3 && \
    BLE_SENSOR_TYPE != BLE_TYPE_GOVEE_V2 && \
    BLE_SENSOR_TYPE != BLE_TYPE_PVVX && \
    BLE_SENSOR_TYPE != BLE_TYPE_BTHOME
    #error "Unknown BLE_SENSOR_TYPE"
#endif

namespace BleSensor {
    void begin();                        // Init NimBLE + start scanning
    void loop(CN105Controller &cn105);   // Keepalive + stale detection

    float    temperature();   // NAN if no data
    float    humidity();      // NAN if not supported
    int8_t   battery();       // -1 if not supported
    int      rssi();          // BLE advertisement RSSI
    bool     isActive();      // Has fresh data
    bool     isStale();       // No data for BLE_STALE_TIMEOUT_MS
    uint32_t lastUpdateAge(); // ms since last reading
    bool     isEnabled();     // Feed toggle (NVS)
    void     setEnabled(bool enabled);
    void     setAddr(const char* mac);   // Update MAC, restart scan
    const char* getAddr();
}

#endif // BLE_SENSOR_TYPE
