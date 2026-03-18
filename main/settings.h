#pragma once

#include <cstdint>
#include <cstring>
#include <nvs_flash.h>
#include "logging.h"
#include "branding.h"
#include "ble_config.h"

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
#ifdef BLE_ENABLE
    char     bleSensorAddr[18] = "";   // "AA:BB:CC:DD:EE:FF" or empty
    bool     bleFeedEnabled = true;    // Feed BLE temp to heat pump
    uint16_t bleStaleTimeoutS = 90;    // Seconds before sensor marked stale (30-600)
#endif
};

class SettingsStore {
public:
    void begin();
    void save();
    DeviceSettings& get() { return _settings; }
    const DeviceSettings& get() const { return _settings; }
private:
    nvs_handle_t _handle = 0;
    DeviceSettings _settings;
};

extern SettingsStore settings;
