#include "settings.h"
#include "cn105_protocol.h"
#include "sl2_proto.h"

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

    // 2026-08: bleFeed -> roomSrc. Not a plain rename (KEY_MIGRATIONS above
    // copies a value byte-for-byte under its new name) — bleFeed was a bool
    // and roomSrc is an sl2_room_src enum, so the value needs mapping, not
    // copying, which is why this runs by hand instead of through the table.
    // An installation that was feeding the heat pump from BLE must keep
    // feeding from BLE after this update: a silent flip to the internal
    // thermistor would change how the house is heated without anyone asking
    // for it. Guarded the same way as the KEY_MIGRATIONS loop above: bleFeed
    // is erased only once roomSrc has been confirmed written, so a failed
    // write (full/corrupt NVS, power loss mid-write) leaves bleFeed in place
    // to retry next boot instead of silently losing the BLE feed.
    //
    // bleFeed only ever MEANT "feed from BLE" alongside a configured sensor —
    // it defaulted to 1 and was rewritten on every save(), so a unit that
    // never paired a thermometer still carries bleFeed=1. Mapping that
    // straight through would name BLE as the room source on almost every
    // upgrading unit, pointing at an empty slot. Ask NVS whether a sensor was
    // ever stored (this runs before any field load, so _settings is not
    // populated yet) and only carry the BLE feed forward if one was.
    {
        uint8_t legacyFeed;
        if (nvs_get_u8(_handle, "bleFeed", &legacyFeed) == ESP_OK) {
            bool haveSensor = false;
            {
                char addr[18] = {0};
                size_t len = sizeof(addr);
                if (nvs_get_str(_handle, "bleAddr", addr, &len) == ESP_OK && addr[0])
                    haveSensor = true;
                BleSensorCfg list[ROOM_MAX_BLE_SENSORS] = {};
                size_t blen = sizeof(list);
                if (!haveSensor &&
                    nvs_get_blob(_handle, "bleList", list, &blen) == ESP_OK &&
                    blen == sizeof(list)) {
                    for (int i = 0; i < ROOM_MAX_BLE_SENSORS && !haveSensor; i++) {
                        list[i].addr[sizeof(list[i].addr) - 1] = '\0';
                        if (list[i].addr[0]) haveSensor = true;
                    }
                }
            }
            uint8_t src = (legacyFeed && haveSensor) ? SL2_ROOMSRC_BLE
                                                     : SL2_ROOMSRC_INTERNAL;
            if (nvs_set_u8(_handle, "roomSrc", src) == ESP_OK) {
                nvs_erase_key(_handle, "bleFeed");
                migrated = true;
                LOG_INFO("[Settings] migrated bleFeed -> roomSrc=%u", src);
            } else {
                LOG_WARN("[Settings] roomSrc write failed — bleFeed kept for retry next boot");
            }
        }
    }

    // 2026-08: bleTimeout -> roomTimeout. Independent of the bleFeed
    // migration above and guarded the same way — one write can succeed while
    // the other fails, so each legacy key's erase is gated on its own new
    // key's write succeeding.
    {
        uint16_t legacyTimeout;
        if (nvs_get_u16(_handle, "bleTimeout", &legacyTimeout) == ESP_OK) {
            if (legacyTimeout < 30 || legacyTimeout > 3600) {
                // Out of range: nothing worth carrying forward — roomTimeout
                // falls back to its own default on load — so just drop it.
                nvs_erase_key(_handle, "bleTimeout");
                migrated = true;
            } else if (nvs_set_u16(_handle, "roomTimeout", legacyTimeout) == ESP_OK) {
                nvs_erase_key(_handle, "bleTimeout");
                migrated = true;
                LOG_INFO("[Settings] migrated bleTimeout -> roomTimeout=%u", legacyTimeout);
            } else {
                LOG_WARN("[Settings] roomTimeout write failed — bleTimeout kept for retry next boot");
            }
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

    // roomSource — uint8_t (enum sl2_room_src). Outside BLE_ENABLE: a build
    // without BLE still needs to choose Internal vs Link. Sanitized on load
    // (same reasoning as modeMask above) so Task 14's arbitration logic can
    // trust it everywhere instead of re-checking a corrupted NVS value.
    {
        uint8_t val = SL2_ROOMSRC_INTERNAL;
        nvs_get_u8(_handle, "roomSrc", &val);
        if (val != SL2_ROOMSRC_INTERNAL && val != SL2_ROOMSRC_BLE && val != SL2_ROOMSRC_LINK)
            val = SL2_ROOMSRC_INTERNAL;
        _settings.roomSource = val;
    }

    // roomStaleTimeoutS — uint16_t
    {
        uint16_t val = 600;
        nvs_get_u16(_handle, "roomTimeout", &val);
        _settings.roomStaleTimeoutS = val;
    }
    if (_settings.roomStaleTimeoutS < 30) _settings.roomStaleTimeoutS = 30;
    if (_settings.roomStaleTimeoutS > 3600) _settings.roomStaleTimeoutS = 3600;

    // The BLE sensor list loads BEFORE the blending model below, which needs
    // it: seeding roomSingle from the legacy roomSrc means answering "which
    // BLE slot?", and room_single_from_legacy() can only answer that once the
    // slots are in _settings. Loading it after would make every upgrading
    // unit look like it had no sensors.
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

    // bleSensors — named multi-sensor list, one blob. When the key has never
    // been written (<= 0.2.4 upgrade), slot 0 is seeded from the legacy
    // single-sensor address so the sensor survives the OTA. NUL-termination is
    // forced on load: the blob crossed a firmware boundary.
    {
        BleSensorCfg list[ROOM_MAX_BLE_SENSORS] = {};
        size_t len = sizeof(list);
        if (nvs_get_blob(_handle, "bleList", list, &len) == ESP_OK && len == sizeof(list)) {
            for (int i = 0; i < ROOM_MAX_BLE_SENSORS; i++) {
                list[i].addr[sizeof(list[i].addr) - 1] = '\0';
                list[i].name[sizeof(list[i].name) - 1] = '\0';
                _settings.bleSensors[i] = list[i];
            }
        } else if (_settings.bleSensorAddr[0]) {
            // Same size both sides (bleSensorAddr is NUL-terminated) — plain copy.
            memcpy(_settings.bleSensors[0].addr, _settings.bleSensorAddr,
                   sizeof(_settings.bleSensors[0].addr));
            strncpy(_settings.bleSensors[0].name, "Remote Sensor",
                    sizeof(_settings.bleSensors[0].name) - 1);
        }
        // Invariant both paths share: the legacy key mirrors slot 0. Same
        // size both sides, source NUL-terminated above — plain copy.
        memcpy(_settings.bleSensorAddr, _settings.bleSensors[0].addr,
               sizeof(_settings.bleSensorAddr));
    }

    LOG_INFO("[Settings] BLE: enabled=%s addr=%s feed=%s timeout=%us",
             _settings.bleEnabled ? "ON" : "OFF",
             strlen(_settings.bleSensorAddr) > 0 ? _settings.bleSensorAddr : "(none)",
             _settings.roomSource == SL2_ROOMSRC_BLE ? "ON" : "OFF",
             _settings.roomStaleTimeoutS);
#endif

    // roomMode / roomSingle / roomMembers / roomOffsets — the blending model.
    // All keys are ADDITIVE over the <= 0.2.4 layout: absent keys are seeded
    // from the legacy roomSrc so an OTA upgrade keeps feeding from the same
    // source, and the legacy keys keep being written (see save()) so a
    // downgrade also keeps working. Sanitized on load, same as modeMask.
    {
        uint8_t val = 0;
        nvs_get_u8(_handle, "roomMode", &val);
        _settings.roomMode = (val == 1) ? 1 : 0;
    }
    {
        // Seed from the (already sanitized) legacy enum when never written.
        // room_single_from_legacy() is the availability-aware mapping: BLE
        // resolves to the first CONFIGURED slot, and to internal when there
        // is none — 0.2.4 wrote bleFeed=1 by default whether or not a sensor
        // was ever paired, so a bare "BLE -> slot 0" seed would point most
        // upgrading units at an empty slot.
        uint8_t seed = room_single_from_legacy(_settings.roomSource);
        uint8_t val = seed;
        nvs_get_u8(_handle, "roomSingle", &val);
        if (val >= ROOM_MEMBER_COUNT) val = seed;
        // A STORED pick can name an empty BLE slot too — written by an
        // earlier build, or left behind when a downgrade dropped the sensor
        // list. Same ghost, same fix: the pump would sit on its internal
        // thermistor while the UI showed a remote source. Link is
        // deliberately not checked here: a bonded dial advertises its sensing
        // hardware only after boot, so its availability isn't knowable yet
        // and demoting it would drop a valid selection on every restart.
        if (val >= ROOM_MEMBER_BLE0) {
            bool configured = false;
#ifdef BLE_ENABLE
            int i = val - ROOM_MEMBER_BLE0;
            configured = (i < ROOM_MAX_BLE_SENSORS) && _settings.bleSensors[i].addr[0];
#endif
            if (!configured) val = seed;
        }
        _settings.roomSingle = val;
    }
    {
        uint8_t val = (uint8_t)(1u << _settings.roomSingle);  // pre-check the current source
        nvs_get_u8(_handle, "roomMembers", &val);
        _settings.roomMembers = val & (uint8_t)((1u << ROOM_MEMBER_COUNT) - 1);
    }
    {
        int8_t offs[ROOM_MEMBER_COUNT] = {0};
        size_t len = sizeof(offs);
        if (nvs_get_blob(_handle, "roomOffs", offs, &len) == ESP_OK && len == sizeof(offs)) {
            for (int i = 0; i < ROOM_MEMBER_COUNT; i++)
                _settings.roomOffsets[i] = std::clamp(offs[i],
                    (int8_t)-ROOM_OFFSET_MAX_TENTHS, ROOM_OFFSET_MAX_TENTHS);
        }
    }

    // roomSource is derived state from here on — recompute so a stored value
    // that predates the blending model can't disagree with roomMode/roomSingle.
    _settings.roomSource = room_source_derived(_settings);

    LOG_INFO("[Settings] Loaded: logLevel=%d poll=%lums name=%s unit=%s room mode=%u single=%u members=0x%02X",
             _settings.logLevel, (unsigned long)_settings.pollMs, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C",
             _settings.roomMode, _settings.roomSingle, _settings.roomMembers);
}

uint8_t room_single_from_legacy(uint8_t src) {
    if (src == SL2_ROOMSRC_LINK) return ROOM_MEMBER_LINK;
    if (src == SL2_ROOMSRC_BLE) {
#ifdef BLE_ENABLE
        for (int i = 0; i < ROOM_MAX_BLE_SENSORS; i++)
            if (settings.get().bleSensors[i].addr[0]) return ROOM_MEMBER_BLE0 + i;
#endif
        // "BLE" with no configured slot is not a selection, it's a ghost:
        // feedSlot() would arm the feed state machine on a sensor that does
        // not exist while the UI reported a remote source. Fall back to the
        // one source that always exists. Callers that can reject instead of
        // demote should gate on RoomAvg::legacySrcSelectable() first.
        return ROOM_MEMBER_INTERNAL;
    }
    return ROOM_MEMBER_INTERNAL;
}

uint8_t room_source_derived(const DeviceSettings &s) {
    if (s.roomMode != 0) return SL2_ROOMSRC_INTERNAL;   // averaging: defined mapping
    if (s.roomSingle == ROOM_MEMBER_LINK) return SL2_ROOMSRC_LINK;
    if (s.roomSingle >= ROOM_MEMBER_BLE0) return SL2_ROOMSRC_BLE;
    return SL2_ROOMSRC_INTERNAL;
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
    // Legacy roomSrc stays written (derived) so the dial and a downgrade to
    // the pre-averaging firmware keep their source.
    _settings.roomSource = room_source_derived(_settings);
    nvs_set_u8(_handle, "roomSrc", _settings.roomSource);
    nvs_set_u16(_handle, "roomTimeout", _settings.roomStaleTimeoutS);
    nvs_set_u8(_handle, "roomMode", _settings.roomMode);
    nvs_set_u8(_handle, "roomSingle", _settings.roomSingle);
    nvs_set_u8(_handle, "roomMembers", _settings.roomMembers);
    nvs_set_blob(_handle, "roomOffs", _settings.roomOffsets, sizeof(_settings.roomOffsets));
#ifdef BLE_ENABLE
    nvs_set_u8(_handle, "bleOn", _settings.bleEnabled ? 1 : 0);
    // Legacy single-sensor key mirrors slot 0 (downgrade compatibility).
    // Same size both sides, always NUL-terminated by the setters.
    memcpy(_settings.bleSensorAddr, _settings.bleSensors[0].addr,
           sizeof(_settings.bleSensorAddr));
    _settings.bleSensorAddr[sizeof(_settings.bleSensorAddr) - 1] = '\0';
    nvs_set_str(_handle, "bleAddr", _settings.bleSensorAddr);
    nvs_set_blob(_handle, "bleList", _settings.bleSensors, sizeof(_settings.bleSensors));
#endif
    nvs_commit(_handle);
    _generation++;

    LOG_INFO("[Settings] Saved: logLevel=%d poll=%lums name=%s unit=%s",
             _settings.logLevel, (unsigned long)_settings.pollMs, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C");
}
