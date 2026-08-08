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

struct DeviceSettings {
    LogLevel logLevel    = LOG_LEVEL_INFO;
    uint32_t pollMs      = 2000;
    char     deviceName[32] = BRAND_NAME;
    float    heatingThreshold = 20.0f;  // AUTO mode heating target
    float    coolingThreshold = 25.0f;  // AUTO mode cooling target
    bool     useFahrenheit = false;     // Web UI display unit
    char     setupCode[9] = "";        // HomeKit pairing code (8 digits)
    bool     wifiChangePending = false; // True after WiFi creds changed via web UI (shorter fallback timeout)
    uint8_t  vaneConfig = 2;          // 0=no vanes, 1=vertical only, 2=vertical+horizontal
    uint8_t  modeMask = MODE_CAP_ALL; // MODE_CAP_* bits — operating modes this unit supports (mode_caps.h)
    // Which source drives the heat pump's room temperature. Outside the BLE
    // ifdef on purpose: a build without BLE still chooses Internal vs Link.
    uint8_t  roomSource = 0;              // enum sl2_room_src
    uint16_t roomStaleTimeoutS = 600;     // Seconds before a source is stale (30-3600)
#ifdef BLE_ENABLE
    bool     bleEnabled = false;          // Master BLE on/off (lazy NimBLE init)
    char     bleSensorAddr[18] = "";      // "AA:BB:CC:DD:EE:FF" or empty
#endif
};

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
