#pragma once

#include "ble_config.h"

#ifdef BLE_ENABLE

#include <cstdint>
#include <cstring>
#include "cn105_protocol.h"

// ── Tunables ────────────────────────────────────────────────────────────────
constexpr uint32_t BLE_KEEPALIVE_MS     = 20000;  // Resend temp to HP every 20s
constexpr uint32_t BLE_RESEND_MIN_MS    = 5000;   // Min gap between value-change resends
constexpr int      BLE_MAX_DISCOVERED   = 20;     // Max devices in discovery scan
constexpr uint32_t BLE_DISCOVERY_MS     = 10000;  // Discovery scan duration
constexpr int8_t   BLE_BATT_LOW_PCT     = 20;     // Battery % at/below = low (HomeKit + web UI)

// ── Discovery result ────────────────────────────────────────────────────────
struct BleDiscoveredDevice {
    char addr[18];        // "AA:BB:CC:DD:EE:FF"
    char name[24];        // BLE advertised name (may be empty)
    const char* type;     // Sensor type (string literal)
    int rssi;
    float temperature;    // °C decoded from advertisement (NAN if unavailable)
    float humidity;       // %RH decoded from advertisement (NAN if unavailable)
};

namespace BleSensor {
    void begin();                        // Init NimBLE + start scanning
    void loop(CN105Controller &cn105);   // Keepalive + stale detection

    void setBleEnabled(bool on);         // Master enable/disable (lazy NimBLE init)
    bool isBleEnabled();                 // Current master toggle state

    float    temperature();   // NAN if no data
    float    humidity();      // NAN if not supported
    int8_t   battery();       // -1 if not supported
    int      rssi();          // BLE advertisement RSSI
    bool     isActive();      // Has fresh data
    bool     isStale();       // No data for stale timeout
    bool     isReverted();    // Stale watchdog handed the HP back to its internal sensor
    uint32_t lastUpdateAge(); // ms since last reading
    bool     isEnabled();     // roomSource == SL2_ROOMSRC_BLE: BLE is the
                               // SELECTED room source right now. NOT "is a
                               // sensor configured" — for that, check
                               // isBleEnabled() && getAddr()[0] instead.
    void     setEnabled(bool enabled);   // Persist feed toggle; loop() reacts to the change
    void     setAddr(const char* mac);   // Update MAC ("" clears), reset readings, restart scan
    const char* getAddr();
    const char* sensorType();            // Detected sensor type (nullptr if unknown)

    // Discovery scan
    void startDiscovery();
    bool isDiscovering();
    bool pollDiscoveryUpdate();          // Returns true when new devices found
    bool pollDiscoveryComplete();        // Returns true once when done
    // Copy current results into out (up to max). Returns count; *truncated set
    // when more devices were seen than fit.
    int  discoveryResults(BleDiscoveredDevice* out, int max, bool* truncated);
}

#endif // BLE_ENABLE
