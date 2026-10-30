#include "settings.h"
#include "cn105_protocol.h"
#include "sl2_proto.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

static const char *TAG = "settings";

SettingsStore settings;
static std::atomic<bool> s_factoryResetting{false};
// NVS writes and reset completion share one lock; no driver lock spans flash IO.
static std::mutex s_settingsWriteMutex;

void SettingsStore::beginFactoryReset() {
    s_factoryResetting.store(true);
    // Wait for an in-flight checked write to observe the latch and finish.
    std::lock_guard<std::mutex> lock(s_settingsWriteMutex);
}

bool SettingsStore::isFactoryResetting() {
    return s_factoryResetting.load();
}

namespace {
constexpr size_t RANGE_CFG_SIZE = 22, RANGE_CACHE_SIZE = 14;
constexpr uint8_t RANGE_RECORD_VERSION = 1;
void put16(uint8_t* b, int16_t v) {
    b[0] = static_cast<uint16_t>(v) & 0xff;
    b[1] = static_cast<uint16_t>(v) >> 8;
}
int16_t get16(const uint8_t* b) {
    return static_cast<int16_t>(static_cast<uint16_t>(b[0]) |
                                (static_cast<uint16_t>(b[1]) << 8));
}
bool validSetpoint(const SetpointSettings& s) {
    using namespace SetpointRange;
    if (s.lastEncoding > Encoding::Enhanced || s.band.heat2 < 20 ||
        s.band.cool2 > 62 || s.band.cool2 - s.band.heat2 < 4) return false;
    for (int i = 0; i < 3; ++i) {
        const auto& p = s.preferences.mode[i];
        if (p.selection > Selection::Manual) return false;
        // Stored manual intent survives an incompatible subsequent encoding.
        // Deliberate edits are validated against current equipment by the coordinator.
        if (p.selection == Selection::Manual &&
            validateManual(static_cast<Mode>(i), p.manual, Encoding::Unknown) != Result::Ok)
            return false;
    }
    return true;
}
void encodeSetpoint(const SetpointSettings& s, uint8_t* b) {
    using namespace SetpointRange;
    b[0] = RANGE_RECORD_VERSION;
    for (int i = 0; i < 3; ++i) {
        const auto& p = s.preferences.mode[i];
        b[1 + i*5] = static_cast<uint8_t>(p.selection);
        put16(b + 2 + i*5, p.selection == Selection::Manual ? p.manual.min2 : 0);
        put16(b + 4 + i*5, p.selection == Selection::Manual ? p.manual.max2 : 0);
    }
    put16(b + 16, s.band.heat2); put16(b + 18, s.band.cool2);
    b[20] = s.autoOwnerUnit ? 1 : 0;
    b[21] = static_cast<uint8_t>(s.lastEncoding);
}
bool decodeSetpoint(const uint8_t* b, SetpointSettings& out) {
    if (b[0] != RANGE_RECORD_VERSION || b[20] > 1) return false;
    SetpointSettings s;
    for (int i = 0; i < 3; ++i) {
        s.preferences.mode[i] = {static_cast<SetpointRange::Selection>(b[1+i*5]),
                                {get16(b+2+i*5), get16(b+4+i*5)}};
    }
    s.band = {get16(b+16), get16(b+18)};
    s.autoOwnerUnit = b[20] != 0;
    s.lastEncoding = static_cast<SetpointRange::Encoding>(b[21]);
    if (!validSetpoint(s)) return false;
    out = s;
    return true;
}
bool encodeCache(const SetpointRange::Detection& d, uint8_t* b) {
    if (d.validMask & ~7u || d.freshMask & ~d.validMask) return false;
    b[0] = RANGE_RECORD_VERSION; b[1] = d.validMask;
    SetpointRange::Preferences automatic{};
    for (auto& p : automatic.mode) p.selection = SetpointRange::Selection::Automatic;
    auto cached = d; cached.freshMask = 0;
    // Keep raw C9 pairs wider than our envelope. As in discovery, require
    // a usable intersection; an encoding change must not rewrite history.
    auto resolved = SetpointRange::resolve(automatic, cached, SetpointRange::Encoding::Enhanced,
                                         SetpointRange::Encoding::Enhanced, 0);
    for (int i = 0; i < 3; ++i) {
        auto r = d.mode[i];
        if (d.validMask & (1u << i)) {
            if (r.min2 <= 0 || r.max2 >= 127 ||
                resolved.mode[i].source != SetpointRange::Source::Cached) return false;
        } else r = {0, 0};
        put16(b+2+i*4, r.min2); put16(b+4+i*4, r.max2);
    }
    return true;
}
// Preserve the exact record, including versions this firmware cannot decode.
// ESP-IDF currently writes on nvs_set_blob; nvs_close is not rollback.
bool readRecord(nvs_handle_t handle, const char* key, std::vector<uint8_t>& bytes, bool& present) {
    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle, key, nullptr, &len);
    present = err != ESP_ERR_NVS_NOT_FOUND;
    if (!present) return true;
    if (err != ESP_OK) return false;
    bytes.resize(len);
    return nvs_get_blob(handle, key, bytes.data(), &len) == ESP_OK && len == bytes.size();
}
bool restoreRecord(nvs_handle_t handle, const char* key, const std::vector<uint8_t>& bytes, bool present) {
    esp_err_t err = present ? nvs_set_blob(handle, key, bytes.data(), bytes.size()) : nvs_erase_key(handle, key);
    return err == ESP_OK || (!present && err == ESP_ERR_NVS_NOT_FOUND);
}
// Also discard pending writes on implementations that stage until commit.
void discardPending(nvs_handle_t& handle) {
    nvs_close(handle); handle = 0;
    if (!SettingsStore::isFactoryResetting())
        nvs_open("ac-settings", NVS_READWRITE, &handle);
}
} // namespace

SetpointSettings SettingsStore::setpointSettings() const {
    std::lock_guard<std::mutex> lock(s_settingsWriteMutex);
    return _setpoint;
}
bool SettingsStore::setpointSettingsAvailable() const {
    std::lock_guard<std::mutex> lock(s_settingsWriteMutex);
    return _setpointAvailable;
}
SetpointRange::Detection SettingsStore::detectedRanges() const {
    std::lock_guard<std::mutex> lock(s_settingsWriteMutex);
    return _detected;
}
void SettingsStore::syncSetpointViews() {
    _settings.heatingThreshold = _setpoint.band.heat2 / 2.0f;
    _settings.coolingThreshold = _setpoint.band.cool2 / 2.0f;
    _settings.autoOwnerUnit = _setpoint.autoOwnerUnit;
}
bool SettingsStore::saveSetpointSettings(const SetpointSettings& next) {
    std::lock_guard<std::mutex> lock(s_settingsWriteMutex);
    return saveSetpointSettingsLocked(next);
}
bool SettingsStore::saveSetpointSettingsLocked(const SetpointSettings& next) {
    if (isFactoryResetting() || !_handle || !validSetpoint(next)) return false;
    uint8_t encoded[RANGE_CFG_SIZE], current[RANGE_CFG_SIZE];
    encodeSetpoint(next, encoded); encodeSetpoint(_setpoint, current);
    if (_setpointAvailable && _setpointStored && !memcmp(encoded, current, sizeof(encoded))) {
        syncSetpointViews();
        return true;
    }
    std::vector<uint8_t> previous;
    bool hadRecord = false;
    if (!readRecord(_handle, "rangeCfg", previous, hadRecord)) return false;
    nvs_type_t mirrorType;
    bool hadHeat = nvs_find_key(_handle, "heatThresh", &mirrorType) == ESP_OK;
    bool hadCool = nvs_find_key(_handle, "coolThresh", &mirrorType) == ESP_OK;
    bool hadOwner = nvs_find_key(_handle, "autoOwner", &mirrorType) == ESP_OK;
    float heat = next.band.heat2 / 2.0f, cool = next.band.cool2 / 2.0f;
    // Mirrors precede the one authoritative write. A mirror failure cannot
    // publish a new profile; new firmware never loads mirrors over rangeCfg.
    bool recordAttempted = false;
    bool ok = nvs_set_blob(_handle, "heatThresh", &heat, sizeof(heat)) == ESP_OK &&
        nvs_set_blob(_handle, "coolThresh", &cool, sizeof(cool)) == ESP_OK &&
        nvs_set_u8(_handle, "autoOwner", next.autoOwnerUnit ? 1 : 0) == ESP_OK &&
        !isFactoryResetting();
    if (ok) {
        // NVS may publish the replacement before deleting the old blob fails.
        // An error result therefore still requires restoration of the old record.
        recordAttempted = true;
        ok = nvs_set_blob(_handle, "rangeCfg", encoded, sizeof(encoded)) == ESP_OK &&
            !isFactoryResetting() && nvs_commit(_handle) == ESP_OK && !isFactoryResetting();
    }
    if (!ok) {
        LOG_ERROR("[Settings] setpoint policy persistence failed; active policy retained");
        if (!isFactoryResetting()) {
            bool recordRestored = !recordAttempted || restoreRecord(_handle, "rangeCfg", previous, hadRecord);
            float oldHeat = _setpoint.band.heat2 / 2.0f, oldCool = _setpoint.band.cool2 / 2.0f;
            // Do not leave initialization's new mirror keys behind on failure:
            // a restart must still recognize a fresh namespace as fresh.
            bool mirrorsRestored = hadHeat ?
                nvs_set_blob(_handle, "heatThresh", &oldHeat, sizeof(oldHeat)) == ESP_OK :
                restoreRecord(_handle, "heatThresh", previous, false);
            mirrorsRestored &= hadCool ?
                nvs_set_blob(_handle, "coolThresh", &oldCool, sizeof(oldCool)) == ESP_OK :
                restoreRecord(_handle, "coolThresh", previous, false);
            mirrorsRestored &= hadOwner ?
                nvs_set_u8(_handle, "autoOwner", _setpoint.autoOwnerUnit ? 1 : 0) == ESP_OK :
                restoreRecord(_handle, "autoOwner", previous, false);
            bool restoredCommit = nvs_commit(_handle) == ESP_OK;
            if (recordAttempted && (!recordRestored || !restoredCommit)) {
                _setpointAvailable = false;
                LOG_ERROR("[Settings] rangeCfg restoration unconfirmed; range policy unavailable");
            }
            if (!mirrorsRestored || !restoredCommit)
                LOG_WARN("[Settings] downgrade mirror restoration incomplete");
        }
        discardPending(_handle);
        syncSetpointViews();
        return false;
    }
    _setpoint = next; _setpointAvailable = true; _setpointStored = true;
    syncSetpointViews();
    ++_generation;
    return true;
}
bool SettingsStore::saveDetectedRanges(const SetpointRange::Detection& next) {
    std::lock_guard<std::mutex> lock(s_settingsWriteMutex);
    uint8_t encoded[RANGE_CACHE_SIZE], current[RANGE_CACHE_SIZE];
    if (isFactoryResetting() || !_handle || !encodeCache(next, encoded)) return false;
    encodeCache(_detected, current);
    if (!memcmp(encoded, current, sizeof(encoded))) return true;
    // Cache keys must not turn an uninitialized fresh namespace into an upgrade.
    // Retry its initial policy first; unavailable policy needs deliberate repair.
    if (!_setpointStored && (!_setpointAvailable || !saveSetpointSettingsLocked(_setpoint)))
        return false;
    std::vector<uint8_t> previous;
    bool hadRecord = false;
    if (!readRecord(_handle, "rangeCache", previous, hadRecord)) return false;
    bool written = nvs_set_blob(_handle, "rangeCache", encoded, sizeof(encoded)) == ESP_OK;
    if (!written || isFactoryResetting() || nvs_commit(_handle) != ESP_OK || isFactoryResetting()) {
        LOG_WARN("[Settings] detected range cache persistence failed");
        // Like rangeCfg, a failed write may already have changed the record.
        if (!isFactoryResetting() &&
            (!restoreRecord(_handle, "rangeCache", previous, hadRecord) || nvs_commit(_handle) != ESP_OK))
            LOG_WARN("[Settings] detected range cache restoration unconfirmed");
        discardPending(_handle);
        return false;
    }
    _detected = next;
    _detected.freshMask = 0; // This store holds history; the driver owns freshness.
    return true;
}
bool SettingsStore::saveAutoSingle(bool single) {
    std::lock_guard<std::mutex> lock(s_settingsWriteMutex);
    if (isFactoryResetting() || !_handle) return false;
    if (nvs_set_u8(_handle, "autoSingle", single ? 1 : 0) != ESP_OK || isFactoryResetting() ||
        nvs_commit(_handle) != ESP_OK || isFactoryResetting()) {
        discardPending(_handle);
        return false;
    }
    _settings.autoSingleSetpoint = single;
    ++_generation;
    return true;
}

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

    // Count before any migration writes keys (including schema-less installs).
    size_t oldEntries = 0;
    bool hadSettings = nvs_get_used_entry_count(_handle, &oldEntries) != ESP_OK || oldEntries != 0;
    uint8_t cfg[RANGE_CFG_SIZE]; size_t cfgLen = sizeof(cfg);
    esp_err_t cfgResult = nvs_get_blob(_handle, "rangeCfg", cfg, &cfgLen);
    bool havePolicy = cfgResult == ESP_OK && cfgLen == sizeof(cfg) && decodeSetpoint(cfg, _setpoint);
    bool missingPolicy = cfgResult == ESP_ERR_NVS_NOT_FOUND;
    _setpointAvailable = havePolicy || missingPolicy;
    _setpointStored = havePolicy;
    if (!havePolicy && !missingPolicy)
        LOG_ERROR("[Settings] rangeCfg invalid/unknown version; range policy unavailable until explicit repair");
    if (missingPolicy) {
        for (auto& p : _setpoint.preferences.mode)
            p = {hadSettings ? SetpointRange::Selection::Compatibility : SetpointRange::Selection::Automatic, {0, 0}};
    }
    uint8_t cache[RANGE_CACHE_SIZE]; size_t cacheLen = sizeof(cache);
    if (nvs_get_blob(_handle, "rangeCache", cache, &cacheLen) == ESP_OK &&
        cacheLen == sizeof(cache) && cache[0] == RANGE_RECORD_VERSION) {
        SetpointRange::Detection d{}; d.validMask = cache[1];
        for (int i = 0; i < 3; ++i) d.mode[i] = {get16(cache+2+i*4), get16(cache+4+i*4)};
        uint8_t validated[RANGE_CACHE_SIZE];
        if (encodeCache(d, validated)) _detected = d;
        else LOG_WARN("[Settings] invalid detected range cache ignored");
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

    // cn105Baud — uint32_t, only 2400/9600 survive the load
    {
        uint32_t val = 0;   // stays 0 when the key is absent
        nvs_get_u32(_handle, "cn105Baud", &val);
        _settings.cn105Baud = cn105_baud_load(val);
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

    // trustedHost: string; missing or unreadable reads as none
    {
        size_t len = sizeof(_settings.trustedHost);
        if (nvs_get_str(_handle, "trustHost", _settings.trustedHost, &len) != ESP_OK)
            _settings.trustedHost[0] = '\0';
    }

    if (!havePolicy) {
        // heatingThreshold — float stored as blob
        {
            float val = 20.0f;
            size_t len = sizeof(val);
            if (nvs_get_blob(_handle, "heatThresh", &val, &len) == ESP_OK && len == sizeof(float) &&
                std::isfinite(val)) {
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
            if (nvs_get_blob(_handle, "coolThresh", &val, &len) == ESP_OK && len == sizeof(float) &&
                std::isfinite(val)) {
                _settings.coolingThreshold = std::clamp(val, CN105_TEMP_MIN, CN105_TEMP_MAX);
            }
        }
    }

    // latitude / longitude — night gate location, floats stored as blobs.
    // No clamp on load: writes are clamped at the web edge, and an
    // out-of-range stray only mis-times twilight, it can't crash the math.
    {
        float val = NAN;
        size_t len = sizeof(val);
        if (nvs_get_blob(_handle, "nightLat", &val, &len) == ESP_OK && len == sizeof(float))
            _settings.latitude = val;
        val = NAN; len = sizeof(val);
        if (nvs_get_blob(_handle, "nightLon", &val, &len) == ESP_OK && len == sizeof(float))
            _settings.longitude = val;
    }

    // night ceiling — u8 percent
    {
        uint8_t v = 20;
        if (nvs_get_u8(_handle, "nightCeil", &v) == ESP_OK) _settings.nightCeilPct = v;
    }
    // night gate edge offsets — minutes, i16
    {
        int16_t v = 0;
        if (nvs_get_i16(_handle, "nightDuskOff", &v) == ESP_OK) _settings.nightDuskOffMin = v;
        v = 0;
        if (nvs_get_i16(_handle, "nightDawnOff", &v) == ESP_OK) _settings.nightDawnOffMin = v;
    }

    // useFahrenheit — bool stored as uint8_t
    {
        uint8_t val = 0;
        nvs_get_u8(_handle, "useFahr", &val);
        _settings.useFahrenheit = (val != 0);
    }

    // betaChannel — bool stored as uint8_t. A missing key leaves val at 0, so
    // an existing install reads the struct default and stays on stable; that
    // is why no schema bump is needed for this field.
    {
        uint8_t val = 0;
        nvs_get_u8(_handle, "betaChan", &val);
        _settings.betaChannel = (val != 0);
    }

    // setupCode — string
    {
        size_t len = sizeof(_settings.setupCode);
        nvs_get_str(_handle, "setupCode", _settings.setupCode, &len);
    }

    // hkPaired — additive latch; absent on older firmware defaults unpaired.
    {
        uint8_t val = 0;
        nvs_get_u8(_handle, "hkPaired", &val);
        _settings.homekitPaired = (val != 0);
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
    // Same contract as roomSource: derived from (roomMode, roomSingle) on load
    // and on every save. The stored value only carries the one thing that pair
    // cannot express — which Link dial a per-dial pin selected.
    nvs_get_u64(_handle, "roomSrcId", &_settings.roomSourceId);
    _settings.roomSourceId = room_source_id_derived(_settings);

    // remoteFed — has this device ever fed the pump a remote temperature?
    // Gates the first-connect clear: only a controller that wrote the register
    // can have left a stale value in it, and on a unit whose wall controller
    // owns that register a clear would take its room sensor away. Latched by
    // main.cpp on the first send. Absent on older firmware: a stored remote
    // selection means this device has been feeding, so derive it from that.
    {
        uint8_t val = 0;
        if (nvs_get_u8(_handle, "remoteFed", &val) == ESP_OK) {
            _settings.remoteEverFed = (val != 0);
        } else {
            _settings.remoteEverFed = (_settings.roomMode != 0) ||
                                      (_settings.roomSingle != ROOM_MEMBER_INTERNAL);
        }
    }

    if (!havePolicy) {
        // autoOwner — who last set the Auto operating point. Absent (older
        // firmware, factory reset): this device, the pre-0.2.9 behaviour.
        {
            uint8_t val = 0;
            if (nvs_get_u8(_handle, "autoOwner", &val) == ESP_OK)
                _settings.autoOwnerUnit = (val != 0);
        }
    }
    // autoSingle — Auto setpoint mode. Absent (older firmware, factory
    // reset): Range, the pre-0.2.9 behaviour.
    {
        uint8_t val = 0;
        if (nvs_get_u8(_handle, "autoSingle", &val) == ESP_OK)
            _settings.autoSingleSetpoint = (val != 0);
        _autoSingleBoot = _settings.autoSingleSetpoint;
    }

    if (missingPolicy) {
        _setpoint.band = {static_cast<int16_t>(std::lround(_settings.heatingThreshold * 2)),
                          static_cast<int16_t>(std::lround(_settings.coolingThreshold * 2))};
        _setpoint.autoOwnerUnit = _settings.autoOwnerUnit;
        // Old versions could retain an invalid gap; normalize migration only.
        if (_setpoint.band.cool2 - _setpoint.band.heat2 < 4) {
            _setpoint.band.heat2 = std::min<int16_t>(_setpoint.band.heat2, 57);
            _setpoint.band.cool2 = _setpoint.band.heat2 + 4;
        }
        saveSetpointSettings(_setpoint);
    }
    if (havePolicy || missingPolicy) syncSetpointViews();

    LOG_INFO("[Settings] Loaded: logLevel=%d poll=%lums baud=%lu name=%s unit=%s room mode=%u single=%u members=0x%02X",
             _settings.logLevel, (unsigned long)_settings.pollMs,
             (unsigned long)_settings.cn105Baud, _settings.deviceName,
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

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Hand-parsed rather than sscanf'd: this runs on the ESP-NOW link path, and
// newlib's scanf engine costs ~960 bytes of stack for a 12-nibble parse.
// Stricter than the sscanf form too — it requires exactly two hex digits per
// group, so a short group like "A:BB:.." is rejected instead of silently
// yielding a different MAC.
bool mac_from_str(const char *s, uint8_t out[6]) {
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(s[i * 3]), lo = hex_nibble(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && s[i * 3 + 2] != ':') return false;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

// The canonical SENSOR-namespace source id for a slot, or 0 if the slot is
// empty/malformed. Shared with espnow_link.cpp via settings.h so the catalog
// and the stored selection can never disagree on a sensor's identity.
uint64_t room_sensor_id_from_addr(const char *s) {
    uint8_t mac[6];
    if (!mac_from_str(s, mac)) return 0;
    return sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_SENSOR, mac);
}

uint64_t room_source_id_derived(const DeviceSettings &s) {
    if (s.roomMode != 0) return SL2_ROOM_SOURCE_AVERAGE_ID;
    if (s.roomSingle == ROOM_MEMBER_LINK) {
        // A per-dial pin (NS_LINK) is the only selection roomSingle cannot
        // encode, so it is the only stored id worth preserving — and only for
        // as long as the selection stays on Link. Anything else means the pin
        // is gone (forgotten dial, source changed elsewhere).
        uint8_t pin[6];
        if (sl2_room_source_id_mac(s.roomSourceId, SL2_ROOM_SOURCE_NS_LINK, pin))
            return s.roomSourceId;
        // Unpinned Link (pre-2026-09 store, or the pinned dial was forgotten):
        // there is no automatic mode any more, so this resolves to the heat
        // pump's own sensor until a Link is chosen again.
        return SL2_ROOM_SOURCE_INTERNAL_ID;
    }
#ifdef BLE_ENABLE
    int i = s.roomSingle - ROOM_MEMBER_BLE0;
    if (i >= 0 && i < ROOM_MAX_BLE_SENSORS)
        return room_sensor_id_from_addr(s.bleSensors[i].addr);
#endif
    return SL2_ROOM_SOURCE_INTERNAL_ID;
}

uint8_t room_source_derived(const DeviceSettings &s) {
    if (s.roomMode != 0) return SL2_ROOMSRC_INTERNAL;   // averaging: defined mapping
    if (s.roomSingle == ROOM_MEMBER_LINK) return SL2_ROOMSRC_LINK;
    if (s.roomSingle >= ROOM_MEMBER_BLE0) return SL2_ROOMSRC_BLE;
    return SL2_ROOMSRC_INTERNAL;
}

void SettingsStore::save() {
    std::lock_guard<std::mutex> lock(s_settingsWriteMutex);
    if (isFactoryResetting() || !_handle) return;
    uint32_t before = _generation;
    if (_setpointAvailable) {
        SetpointSettings next = _setpoint;
        float heat = _settings.heatingThreshold, cool = _settings.coolingThreshold;
        if (!std::isfinite(heat) || !std::isfinite(cool) || heat < 10 || cool > 31 ||
            heat * 2 != std::round(heat * 2) || cool * 2 != std::round(cool * 2)) {
            syncSetpointViews(); return;
        }
        next.band = {static_cast<int16_t>(heat * 2), static_cast<int16_t>(cool * 2)};
        next.autoOwnerUnit = _settings.autoOwnerUnit;
        if (!saveSetpointSettingsLocked(next)) {syncSetpointViews(); return;}
    }
    nvs_set_u8(_handle, "schemaVer", SETTINGS_SCHEMA_VERSION);
    nvs_set_u8(_handle, "logLevel", _settings.logLevel);
    nvs_set_u32(_handle, "pollMs", _settings.pollMs);
    nvs_set_u32(_handle, "cn105Baud", _settings.cn105Baud);
    nvs_set_str(_handle, "deviceName", _settings.deviceName);
    nvs_set_str(_handle, "trustHost", _settings.trustedHost);
    nvs_set_blob(_handle, "nightLat", &_settings.latitude, sizeof(float));
    nvs_set_blob(_handle, "nightLon", &_settings.longitude, sizeof(float));
    nvs_set_u8(_handle, "nightCeil", _settings.nightCeilPct);
    nvs_set_i16(_handle, "nightDuskOff", _settings.nightDuskOffMin);
    nvs_set_i16(_handle, "nightDawnOff", _settings.nightDawnOffMin);
    nvs_set_u8(_handle, "useFahr", _settings.useFahrenheit ? 1 : 0);
    nvs_set_u8(_handle, "betaChan", _settings.betaChannel ? 1 : 0);
    nvs_set_str(_handle, "setupCode", _settings.setupCode);
    nvs_set_u8(_handle, "hkPaired", _settings.homekitPaired ? 1 : 0);
    nvs_set_u8(_handle, "remoteFed", _settings.remoteEverFed ? 1 : 0);
    nvs_set_u8(_handle, "autoSingle", _settings.autoSingleSetpoint ? 1 : 0);
    nvs_set_u8(_handle, "wifiChgPend", _settings.wifiChangePending ? 1 : 0);
    nvs_set_u8(_handle, "vaneConfig", _settings.vaneConfig);
    nvs_set_u8(_handle, "modeMask", _settings.modeMask);
    // Legacy roomSrc stays written (derived) so the dial and a downgrade to
    // the pre-averaging firmware keep their source.
    _settings.roomSource = room_source_derived(_settings);
    nvs_set_u8(_handle, "roomSrc", _settings.roomSource);
    // Derived too, so every writer of roomMode/roomSingle — the web UI, a dial
    // edit, a removed BLE slot — gets a consistent id without restating it.
    _settings.roomSourceId = room_source_id_derived(_settings);
    nvs_set_u64(_handle, "roomSrcId", _settings.roomSourceId);
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
    if (isFactoryResetting() || nvs_commit(_handle) != ESP_OK || isFactoryResetting()) {
        discardPending(_handle); return;
    }
    _generation = before + 1;

    LOG_INFO("[Settings] Saved: logLevel=%d poll=%lums baud=%lu name=%s unit=%s",
             _settings.logLevel, (unsigned long)_settings.pollMs,
             (unsigned long)_settings.cn105Baud, _settings.deviceName,
             _settings.useFahrenheit ? "F" : "C");
}
