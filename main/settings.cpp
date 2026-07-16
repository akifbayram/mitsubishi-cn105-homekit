#include "settings.h"
#include "cn105_protocol.h"

#include <algorithm>

static const char *TAG = "settings";

SettingsStore settings;

// Key renames (RevK `.old=` pattern): on boot, if newKey is absent and oldKey
// holds a value, the value is copied (type read from NVS, so entries need no
// type column) and the old key erased. Sentinel-terminated — a zero-length
// array is ill-formed C++. Example entry:
//   { "useFahr", "useFahrenheit" },
struct KeyMigration { const char *oldKey; const char *newKey; };
static const KeyMigration KEY_MIGRATIONS[] = {
    { nullptr, nullptr },  // sentinel
};

void SettingsStore::runKeyMigrations() {
    bool migrated = false;
    for (const KeyMigration *m = KEY_MIGRATIONS; m->oldKey; m++) {
        nvs_type_t type;
        if (nvs_find_key(_handle, m->newKey, &type) == ESP_OK) continue;  // already migrated
        if (nvs_find_key(_handle, m->oldKey, &type) != ESP_OK) continue;  // nothing stored
        esp_err_t err = ESP_FAIL;
        switch (type) {
            case NVS_TYPE_U8:  { uint8_t v;  if (nvs_get_u8(_handle, m->oldKey, &v) == ESP_OK)  err = nvs_set_u8(_handle, m->newKey, v);  break; }
            case NVS_TYPE_U16: { uint16_t v; if (nvs_get_u16(_handle, m->oldKey, &v) == ESP_OK) err = nvs_set_u16(_handle, m->newKey, v); break; }
            case NVS_TYPE_U32: { uint32_t v; if (nvs_get_u32(_handle, m->oldKey, &v) == ESP_OK) err = nvs_set_u32(_handle, m->newKey, v); break; }
            case NVS_TYPE_STR: {
                char v[64]; size_t len = sizeof(v);
                if (nvs_get_str(_handle, m->oldKey, v, &len) == ESP_OK)
                    err = nvs_set_str(_handle, m->newKey, v);
                break;
            }
            case NVS_TYPE_BLOB: {
                uint8_t v[16]; size_t len = sizeof(v);  // largest blob today: 4-byte float
                if (nvs_get_blob(_handle, m->oldKey, v, &len) == ESP_OK)
                    err = nvs_set_blob(_handle, m->newKey, v, len);
                break;
            }
            default:
                LOG_WARN("[Settings] key migration %s->%s: unhandled NVS type %d",
                         m->oldKey, m->newKey, (int)type);
                break;
        }
        if (err == ESP_OK) {
            nvs_erase_key(_handle, m->oldKey);
            migrated = true;
            LOG_INFO("[Settings] migrated NVS key %s -> %s", m->oldKey, m->newKey);
        }
    }
    if (migrated) nvs_commit(_handle);
}

void SettingsStore::begin() {
    esp_err_t err = nvs_open("ac-settings", NVS_READWRITE, &_handle);
    if (err != ESP_OK) {
        LOG_ERROR("[Settings] nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    // Key renames move stored values to their new names before any field load
    runKeyMigrations();

    // Schema version — no migrations exist yet; read it so future versions
    // can branch on what layout this device last wrote (see settings.h).
    {
        uint8_t storedVer = 0;
        nvs_get_u8(_handle, "schemaVer", &storedVer);
        if (storedVer > SETTINGS_SCHEMA_VERSION) {
            LOG_WARN("[Settings] stored schema v%u is newer than firmware's v%u (downgrade?)",
                     storedVer, SETTINGS_SCHEMA_VERSION);
        }
    }

    // logLevel — uint8_t. DEBUG is session-only: a debug session that ends
    // badly (crash, WiFi loss) must not leave the device booting into the
    // firehose — DEBUG + a WS client can starve WiFi within a minute (see
    // WS_LOG_SHED_HEAP in web_ws.cpp). Same sanitize-on-load pattern as
    // modeMask below.
    {
        uint8_t val = LOG_LEVEL_INFO;
        nvs_get_u8(_handle, "logLevel", &val);
        if (val > LOG_LEVEL_INFO) val = LOG_LEVEL_INFO;
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
            // Clamp: firmware <= 0.2.4 allowed 31.0, above today's HAP char max
            // (30.5) — an out-of-range value makes hap_char_update_val reject
            // every threshold sync.
            _settings.heatingThreshold = std::clamp(val, CN105_TEMP_MIN, CN105_TEMP_MAX);
        }
    }

    // coolingThreshold — float stored as blob
    {
        float val = 25.0f;
        size_t len = sizeof(val);
        if (nvs_get_blob(_handle, "coolThresh", &val, &len) == ESP_OK && len == sizeof(float)) {
            _settings.coolingThreshold = std::clamp(val, CN105_TEMP_MIN, CN105_TEMP_MAX);
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

    // modeMask — uint8_t (sanitized here so consumers can trust it everywhere)
    {
        uint8_t val = MODE_CAP_ALL;
        nvs_get_u8(_handle, "modeMask", &val);
        _settings.modeMask = mode_mask_sanitize(val);
    }

#ifdef BLE_ENABLE
    // bleEnabled — bool stored as uint8_t
    // First-boot default honors the -DBLE_SENSOR_DEFAULT_ON build flag; a stored
    // NVS value (set via web UI) overrides it on subsequent boots.
    {
#ifdef BLE_SENSOR_DEFAULT_ON
        uint8_t val = 1;
#else
        uint8_t val = 0;
#endif
        nvs_get_u8(_handle, "bleOn", &val);
        _settings.bleEnabled = (val != 0);
    }

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
        uint16_t val = 600;
        nvs_get_u16(_handle, "bleTimeout", &val);
        _settings.bleStaleTimeoutS = val;
    }
    if (_settings.bleStaleTimeoutS < 30) _settings.bleStaleTimeoutS = 30;
    if (_settings.bleStaleTimeoutS > 3600) _settings.bleStaleTimeoutS = 3600;

    LOG_INFO("[Settings] BLE: enabled=%s addr=%s feed=%s timeout=%us",
             _settings.bleEnabled ? "ON" : "OFF",
             strlen(_settings.bleSensorAddr) > 0 ? _settings.bleSensorAddr : "(none)",
             _settings.bleFeedEnabled ? "ON" : "OFF",
             _settings.bleStaleTimeoutS);
#endif

    LOG_INFO("[Settings] Loaded: logLevel=%d poll=%lums name=%s unit=%s",
             _settings.logLevel, (unsigned long)_settings.pollMs, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C");
}

void SettingsStore::save() {
    nvs_set_u8(_handle, "schemaVer", SETTINGS_SCHEMA_VERSION);
    nvs_set_u8(_handle, "logLevel", _settings.logLevel);
    nvs_set_u32(_handle, "pollMs", _settings.pollMs);
    nvs_set_str(_handle, "deviceName", _settings.deviceName);
    nvs_set_blob(_handle, "heatThresh", &_settings.heatingThreshold, sizeof(float));
    nvs_set_blob(_handle, "coolThresh", &_settings.coolingThreshold, sizeof(float));
    nvs_set_u8(_handle, "useFahr", _settings.useFahrenheit ? 1 : 0);
    nvs_set_str(_handle, "setupCode", _settings.setupCode);
    nvs_set_u8(_handle, "wifiChgPend", _settings.wifiChangePending ? 1 : 0);
    nvs_set_u8(_handle, "vaneConfig", _settings.vaneConfig);
    nvs_set_u8(_handle, "modeMask", _settings.modeMask);
#ifdef BLE_ENABLE
    nvs_set_u8(_handle, "bleOn", _settings.bleEnabled ? 1 : 0);
    nvs_set_str(_handle, "bleAddr", _settings.bleSensorAddr);
    nvs_set_u8(_handle, "bleFeed", _settings.bleFeedEnabled ? 1 : 0);
    nvs_set_u16(_handle, "bleTimeout", _settings.bleStaleTimeoutS);
#endif
    nvs_commit(_handle);
    _generation++;

    LOG_INFO("[Settings] Saved: logLevel=%d poll=%lums name=%s unit=%s",
             _settings.logLevel, (unsigned long)_settings.pollMs, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C");
}
