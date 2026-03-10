#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "logging.h"

struct DeviceSettings {
    LogLevel logLevel    = LOG_LEVEL_INFO;
    uint32_t pollMs      = 2000;
    char     deviceName[32] = "Mitsubishi Mini Split";
    float    heatingThreshold = 20.0f;  // AUTO mode heating target
    float    coolingThreshold = 25.0f;  // AUTO mode cooling target
    bool     useFahrenheit = false;     // Web UI display unit
    char     setupCode[9] = "";        // HomeKit pairing code (8 digits)
    bool     wifiChangePending = false; // True after WiFi creds changed via web UI (shorter fallback timeout)
    uint8_t  vaneConfig = 2;          // 0=no vanes, 1=vertical only, 2=vertical+horizontal
};

class SettingsStore {
public:
    void begin();
    void save();
    DeviceSettings& get() { return _settings; }
    const DeviceSettings& get() const { return _settings; }
private:
    Preferences _prefs;
    DeviceSettings _settings;
};

extern SettingsStore settings;
