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

    // ── Per-slot readings (idx = 0..ROOM_MAX_BLE_SENSORS-1, settings list) ──
    float    temperature(int idx);   // NAN if no data
    float    humidity(int idx);      // NAN if not supported
    int8_t   battery(int idx);       // -1 if not supported
    int      rssi(int idx);          // BLE advertisement RSSI
    bool     isActive(int idx);      // Has fresh data
    bool     isStale(int idx);       // No data for stale timeout
    uint32_t lastUpdateAge(int idx); // ms since last reading; UINT32_MAX if none
    const char* sensorType(int idx); // Detected type (nullptr if unknown)
    bool     isConfigured(int idx);  // Slot has a valid address

    // The one sensor that stands for "the remote sensor" where only one can be
    // named: the slot feeding the heat pump when there is one, else the first
    // configured slot. -1 when nothing is configured. Slot 0 is NOT that
    // sensor — it can be empty while slots 1..3 hold real ones, and pinning
    // these consumers to it made the HomeKit accessory disappear on a
    // slot-0 delete and sent the dial one sensor's humidity beside another's
    // temperature.
    int      primarySlot();

    // No-arg accessors follow primarySlot() (HomeKit sensor accessory, dial
    // battery TLV — both predate the multi-sensor list and can only show one).
    inline float    temperature()   { return temperature(primarySlot()); }
    inline float    humidity()      { return humidity(primarySlot()); }
    inline int8_t   battery()       { return battery(primarySlot()); }
    inline int      rssi()          { return rssi(primarySlot()); }
    inline bool     isActive()      { return isActive(primarySlot()); }
    inline bool     isStale()       { return isStale(primarySlot()); }
    inline uint32_t lastUpdateAge() { return lastUpdateAge(primarySlot()); }
    inline const char* sensorType() { return sensorType(primarySlot()); }

    bool     isReverted();    // Stale watchdog handed the HP back to its internal sensor
    // Which slot the single-mode feed reads from; -1 when averaging or a
    // non-BLE source is selected. Safe to pass straight into the indexed
    // accessors — they range-check.
    int      feedSlot();
    bool     isEnabled();     // a BLE slot is the SELECTED single-mode room
                               // source right now. NOT "is a sensor
                               // configured" — for that, check
                               // isBleEnabled() && isConfigured(i) instead.

    // Sensor list management. mac "" clears the slot (readings reset, scan
    // restarted); name nullptr keeps the stored name.
    void     setSensor(int idx, const char* mac, const char* name);
    void     renameSensor(int idx, const char* name);
    // Legacy single-sensor web command — the inverse of getAddr(), so it edits
    // the slot that call reports rather than slot 0: that slot can be empty
    // while another holds the sensor the old UI is showing, and writing there
    // would add a ghost and strand the real one. Nothing configured yet ->
    // slot 0, where the first sensor belongs.
    inline void setAddr(const char* mac) {
        int idx = primarySlot();
        setSensor(idx >= 0 ? idx : 0, mac, nullptr);
    }
    const char* getAddr();               // primarySlot() address, "" when none

    // Discovery scan
    void startDiscovery();
    bool isDiscovering();
    bool pollDiscoveryUpdate();          // Returns true when new devices found
    bool pollDiscoveryComplete();        // Returns true once when done
    // Copy current results into out (up to max). Returns count; *truncated set
    // when more devices were seen than fit. Returns 0 (and clears *truncated)
    // when a new scan wipes the list mid-copy — never a mix of two scans.
    int  discoveryResults(BleDiscoveredDevice* out, int max, bool* truncated);
}

#endif // BLE_ENABLE
