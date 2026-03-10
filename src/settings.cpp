#include "settings.h"

SettingsStore settings;

void SettingsStore::begin() {
    _prefs.begin("ac-settings", false);  // false = read-write

    // Load saved values (use defaults if not set)
    _settings.logLevel = (LogLevel)_prefs.getUChar("logLevel", LOG_LEVEL_INFO);
    _settings.pollMs = _prefs.getUInt("pollMs", 2000);

    size_t nameLen = _prefs.getString("deviceName", _settings.deviceName, sizeof(_settings.deviceName));
    if (nameLen == 0) {
        strncpy(_settings.deviceName, "Mitsubishi Mini Split", sizeof(_settings.deviceName) - 1);
        _settings.deviceName[sizeof(_settings.deviceName) - 1] = '\0';
    }

    _settings.heatingThreshold = _prefs.getFloat("heatThresh", 20.0f);
    _settings.coolingThreshold = _prefs.getFloat("coolThresh", 25.0f);
    _settings.useFahrenheit = _prefs.getBool("useFahr", false);
    _prefs.getString("setupCode", _settings.setupCode, sizeof(_settings.setupCode));
    _settings.wifiChangePending = _prefs.getBool("wifiChgPend", false);
    _settings.vaneConfig = _prefs.getUChar("vaneConfig", 2);

    LOG_INFO("[Settings] Loaded: logLevel=%d poll=%lums name=%s unit=%s",
             _settings.logLevel, _settings.pollMs, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C");
}

void SettingsStore::save() {
    _prefs.putUChar("logLevel", _settings.logLevel);
    _prefs.putUInt("pollMs", _settings.pollMs);
    _prefs.putString("deviceName", _settings.deviceName);
    _prefs.putFloat("heatThresh", _settings.heatingThreshold);
    _prefs.putFloat("coolThresh", _settings.coolingThreshold);
    _prefs.putBool("useFahr", _settings.useFahrenheit);
    _prefs.putString("setupCode", _settings.setupCode);
    _prefs.putBool("wifiChgPend", _settings.wifiChangePending);
    _prefs.putUChar("vaneConfig", _settings.vaneConfig);
    LOG_INFO("[Settings] Saved: logLevel=%d poll=%lums name=%s unit=%s",
             _settings.logLevel, _settings.pollMs, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C");
}
