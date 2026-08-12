#pragma once

#include <cstdint>
#include <cstring>
#include <nvs_flash.h>
#include "logging.h"
#include "branding.h"
#include "ble_config.h"
#include "mode_caps.h"

// Bump when a stored key's meaning or encoding changes, and add a
// `if (storedVer < N) { ...migrate... }` branch in SettingsStore::begin().
// The version has been written since it was introduced, so a device that
// reports 0 predates it (treat as "oldest known layout"). Adding a NEW key
// with a sane default does NOT need a bump — absent keys already fall back
// to defaults on load.
//
// RENAMING a key needs neither a bump nor a branch: add {"oldKey","newKey"}
// to KEY_MIGRATIONS in settings.cpp (RevK `.old=` pattern). On the next boot
// the stored value moves to the new key (type-generic copy) and the old key
// is erased — existing devices keep the setting instead of silently
// resetting to the default.
inline constexpr uint8_t SETTINGS_SCHEMA_VERSION = 1;

// ── Room-sensor blending ────────────────────────────────────────────────────
// Stable member-bit assignment shared by roomMembers, the roomOffsets index,
// the web UI, and NVS. Never renumber: the mask and offsets persist.
//   bit 0        = heat pump internal thermistor
//   bit 1        = Serin Link dial sensor
//   bit 2 + i    = BLE sensor slot i
inline constexpr int ROOM_MAX_BLE_SENSORS = 4;
inline constexpr int ROOM_MEMBER_INTERNAL = 0;
inline constexpr int ROOM_MEMBER_LINK     = 1;
inline constexpr int ROOM_MEMBER_BLE0     = 2;
inline constexpr int ROOM_MEMBER_COUNT    = ROOM_MEMBER_BLE0 + ROOM_MAX_BLE_SENSORS;
inline constexpr int8_t ROOM_OFFSET_MAX_TENTHS = 99;   // ±9.9 °C

// One configured BLE sensor. addr empty = free slot. Fixed-size so the whole
// list round-trips NVS as a single blob.
struct BleSensorCfg {
    char addr[18];   // "AA:BB:CC:DD:EE:FF" or "" (empty slot)
    char name[24];   // user-assigned display name
};

struct DeviceSettings {
    LogLevel logLevel    = LOG_LEVEL_INFO;
    uint32_t pollMs      = 2000;
    char     deviceName[32] = BRAND_NAME;
    float    heatingThreshold = 20.0f;  // AUTO mode heating target
    float    coolingThreshold = 25.0f;  // AUTO mode cooling target
    bool     useFahrenheit = false;     // Web UI display unit
    bool     betaChannel = false;      // Update check also reads the beta manifest
    char     setupCode[9] = "";        // HomeKit pairing code (8 digits)
    bool     wifiChangePending = false; // True after WiFi creds changed via web UI (shorter fallback timeout)
    uint8_t  vaneConfig = 2;          // 0=no vanes, 1=vertical only, 2=vertical+horizontal
    uint8_t  modeMask = MODE_CAP_ALL; // MODE_CAP_* bits — operating modes this unit supports (mode_caps.h)
    // Which source drives the heat pump's room temperature. Outside the BLE
    // ifdef on purpose: a build without BLE still chooses Internal vs Link.
    // DERIVED from (roomMode, roomSingle) on every save — kept stored so the
    // Serin Link dial (sl2_room_src) and a firmware downgrade keep working.
    uint8_t  roomSource = 0;              // enum sl2_room_src
    uint16_t roomStaleTimeoutS = 600;     // Seconds before a source is stale (30-3600)
    // Blending model. Single mode reproduces the legacy one-source behavior;
    // Average mode feeds the equal-weight mean of the checked members.
    uint8_t  roomMode = 0;                // 0 = single, 1 = average
    uint8_t  roomSingle = 0;              // single-mode pick, ROOM_MEMBER_* bit index
    uint8_t  roomMembers = 0;             // average-mode membership bitmask
    int8_t   roomOffsets[ROOM_MEMBER_COUNT] = {0};  // tenths °C, ±ROOM_OFFSET_MAX_TENTHS
#ifdef BLE_ENABLE
    bool     bleEnabled = false;          // Master BLE on/off (lazy NimBLE init)
    // Legacy single-sensor key, kept in lockstep with bleSensors[0].addr so a
    // downgrade to <= the pre-averaging firmware keeps its sensor.
    char     bleSensorAddr[18] = "";      // "AA:BB:CC:DD:EE:FF" or empty
    BleSensorCfg bleSensors[ROOM_MAX_BLE_SENSORS] = {};
#endif
};

// (roomMode, roomSingle) -> the legacy sl2_room_src enum the dial speaks.
// Averaging maps to Internal by contract (the dial has no "average" concept).
uint8_t room_source_derived(const DeviceSettings &s);

// The inverse, for legacy-enum writers (dial edit, old web command): a pick
// is a single-mode selection; BLE maps to the first CONFIGURED slot (slot 0
// may be empty after a remove), or to internal when none is configured.
uint8_t room_single_from_legacy(uint8_t src);

// A member's calibration offset, in tenths °C. Every path that feeds the heat
// pump goes through these — the Average blend AND both single-source paths —
// so an offset means the same thing in either mode. Out-of-range bits read as
// no offset rather than off the end of the array.
inline int room_offset_dc(const DeviceSettings &s, int bit) {
    return (bit >= 0 && bit < ROOM_MEMBER_COUNT) ? s.roomOffsets[bit] : 0;
}
inline float room_apply_offset(const DeviceSettings &s, int bit, float t) {
    return t + room_offset_dc(s, bit) / 10.0f;
}

class SettingsStore {
public:
    void begin();
    void save();
    DeviceSettings& get() { return _settings; }
    const DeviceSettings& get() const { return _settings; }
    // Bumped on every save() — lets consumers poll "did settings change?"
    // with one integer compare instead of per-field compares each loop tick.
    uint32_t generation() const { return _generation; }
private:
    void runKeyMigrations();
    nvs_handle_t _handle = 0;
    DeviceSettings _settings;
    uint32_t _generation = 0;
};

extern SettingsStore settings;
