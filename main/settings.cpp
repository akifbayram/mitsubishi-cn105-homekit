#include "settings.h"

static const char *TAG = "settings";

SettingsStore settings;

void SettingsStore::begin() {
    esp_err_t err = nvs_open("ac-settings", NVS_READWRITE, &_handle);
    if (err != ESP_OK) {
        LOG_ERROR("[Settings] nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    // logLevel — uint8_t
    {
        uint8_t val = LOG_LEVEL_INFO;
        nvs_get_u8(_handle, "logLevel", &val);
        _settings.logLevel = (LogLevel)val;
    }

    // pollMs — uint32_t
    {
        uint32_t val = 2000;
        nvs_get_u32(_handle, "pollMs", &val);
        _settings.pollMs = val;
    }

    // deviceName — string
    {
        size_t len = sizeof(_settings.deviceName);
        esp_err_t ret = nvs_get_str(_handle, "deviceName", _settings.deviceName, &len);
        if (ret != ESP_OK || len == 0) {
            strncpy(_settings.deviceName, BRAND_NAME, sizeof(_settings.deviceName) - 1);
            _settings.deviceName[sizeof(_settings.deviceName) - 1] = '\0';
        }
    }

    // heatingThreshold — float stored as blob
    {
        float val = 20.0f;
        size_t len = sizeof(val);
        if (nvs_get_blob(_handle, "heatThresh", &val, &len) == ESP_OK && len == sizeof(float)) {
            _settings.heatingThreshold = val;
        }
    }

    // coolingThreshold — float stored as blob
    {
        float val = 25.0f;
        size_t len = sizeof(val);
        if (nvs_get_blob(_handle, "coolThresh", &val, &len) == ESP_OK && len == sizeof(float)) {
            _settings.coolingThreshold = val;
        }
    }

    // useFahrenheit — bool stored as uint8_t
    {
        uint8_t val = 0;
        nvs_get_u8(_handle, "useFahr", &val);
        _settings.useFahrenheit = (val != 0);
    }

    // setupCode — string
    {
        size_t len = sizeof(_settings.setupCode);
        nvs_get_str(_handle, "setupCode", _settings.setupCode, &len);
    }

    // wifiChangePending — bool stored as uint8_t
    {
        uint8_t val = 0;
        nvs_get_u8(_handle, "wifiChgPend", &val);
        _settings.wifiChangePending = (val != 0);
    }

    // vaneConfig — uint8_t
    {
        uint8_t val = 2;
        nvs_get_u8(_handle, "vaneConfig", &val);
        _settings.vaneConfig = val;
    }

#ifdef BLE_ENABLE
    // bleSensorAddr — string
    {
        size_t len = sizeof(_settings.bleSensorAddr);
        esp_err_t ret = nvs_get_str(_handle, "bleAddr", _settings.bleSensorAddr, &len);
        if (ret != ESP_OK || len == 0) {
#ifdef BLE_SENSOR_ADDR
            strncpy(_settings.bleSensorAddr, BLE_SENSOR_ADDR, sizeof(_settings.bleSensorAddr) - 1);
            _settings.bleSensorAddr[sizeof(_settings.bleSensorAddr) - 1] = '\0';
#endif
        }
    }

    // bleFeedEnabled — bool stored as uint8_t
    {
        uint8_t val = 1;
        nvs_get_u8(_handle, "bleFeed", &val);
        _settings.bleFeedEnabled = (val != 0);
    }

    // bleStaleTimeoutS — uint16_t
    {
        uint16_t val = 90;
        nvs_get_u16(_handle, "bleTimeout", &val);
        _settings.bleStaleTimeoutS = val;
    }
    if (_settings.bleStaleTimeoutS < 30) _settings.bleStaleTimeoutS = 30;
    if (_settings.bleStaleTimeoutS > 600) _settings.bleStaleTimeoutS = 600;

    LOG_INFO("[Settings] BLE: addr=%s feed=%s timeout=%us",
             strlen(_settings.bleSensorAddr) > 0 ? _settings.bleSensorAddr : "(none)",
             _settings.bleFeedEnabled ? "ON" : "OFF",
             _settings.bleStaleTimeoutS);
#endif

    LOG_INFO("[Settings] Loaded: logLevel=%d poll=%lums name=%s unit=%s",
             _settings.logLevel, (unsigned long)_settings.pollMs, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C");
}

void SettingsStore::save() {
    nvs_set_u8(_handle, "logLevel", _settings.logLevel);
    nvs_set_u32(_handle, "pollMs", _settings.pollMs);
    nvs_set_str(_handle, "deviceName", _settings.deviceName);
    nvs_set_blob(_handle, "heatThresh", &_settings.heatingThreshold, sizeof(float));
    nvs_set_blob(_handle, "coolThresh", &_settings.coolingThreshold, sizeof(float));
    nvs_set_u8(_handle, "useFahr", _settings.useFahrenheit ? 1 : 0);
    nvs_set_str(_handle, "setupCode", _settings.setupCode);
    nvs_set_u8(_handle, "wifiChgPend", _settings.wifiChangePending ? 1 : 0);
    nvs_set_u8(_handle, "vaneConfig", _settings.vaneConfig);
#ifdef BLE_ENABLE
    nvs_set_str(_handle, "bleAddr", _settings.bleSensorAddr);
    nvs_set_u8(_handle, "bleFeed", _settings.bleFeedEnabled ? 1 : 0);
    nvs_set_u16(_handle, "bleTimeout", _settings.bleStaleTimeoutS);
#endif
    nvs_commit(_handle);

    LOG_INFO("[Settings] Saved: logLevel=%d poll=%lums name=%s unit=%s",
             _settings.logLevel, (unsigned long)_settings.pollMs, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C");
}
