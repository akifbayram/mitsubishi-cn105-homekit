#pragma once
// SwitchBot / Govee / PVVX / BTHome BLE advertisement decoders.
//
// Split out of ble_sensor.cpp so it compiles standalone (see test/ble_decoders/).
// Pure logic — no NimBLE/FreeRTOS/ESP dependencies. Consumed by the scan
// callback in ble_sensor.cpp; decoders fill a SensorReading and callers merge.

#include <cstdint>
#include <cstring>
#include <strings.h>
#include <cmath>
#include <cstddef>

// Per-MAC SwitchBot type cache capacity (see resolveSwitchBotType).
static constexpr int SB_TYPE_CACHE_SIZE = 8;
// Monotonic tick for cache LRU ordering — only relative order matters.
static uint32_t s_sbTypeTick = 0;

static inline bool validTemp(float t) { return t >= -40.0f && t <= 80.0f; }
static inline bool validHum(float h)  { return h >= 0.0f && h <= 100.0f; }
static inline bool validBatt(int8_t b){ return b >= 0 && b <= 100; }

// ── Decoded sensor reading — filled by decoders (NAN/-1 mean "not present") ──
struct SensorReading {
    float  temp = NAN;
    float  hum  = NAN;
    int8_t batt = -1;
};

// ══════════════════════════════════════════════════════════════════════════════
// Decoders — all compiled in, dispatched at runtime
// ══════════════════════════════════════════════════════════════════════════════

// Shared Govee 3-byte combined temp+hum decoder (V3 offset 3, V1 offset 4)
static bool decodeGoveeCombined(const uint8_t* data, uint8_t offset, SensorReading& out) {
    int32_t val = ((int32_t)data[offset] << 16)
                | ((int32_t)data[offset+1] << 8)
                | data[offset+2];
    bool negative = (val & 0x800000);
    if (negative) val ^= 0x800000;
    float temp = (float)(val / 1000) / 10.0f;
    if (negative) temp = -temp;
    float hum = (float)(val % 1000) / 10.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    out.temp = temp;
    out.hum = hum;
    return true;
}

// Govee H5072/H5075 — 3-byte combined temp+hum encoding
// Manufacturer data 0xEC88: [0-1]=company ID, [2]=padding, [3-5]=combined, [6]=battery|error
static bool decodeGoveeV3(const uint8_t* mfr, uint8_t len, SensorReading& out) {
    if (len < 7 || (mfr[6] & 0x80)) return false;
    if (!decodeGoveeCombined(mfr, 3, out)) return false;
    out.batt = (int8_t)(mfr[6] & 0x7F);
    return true;
}

// Govee H5074/H5051/H5052/H5071 — little-endian temp/hum
// Manufacturer data 0xEC88: [0-1]=company ID, [2]=reserved, [3-4]=temp LE, [5-6]=hum LE, [7]=battery
static bool decodeGoveeV2(const uint8_t* mfr, uint8_t len, SensorReading& out) {
    if (len < 8) return false;
    int16_t rawTemp = (int16_t)(mfr[3] | (mfr[4] << 8));
    uint16_t rawHum = (uint16_t)(mfr[5] | (mfr[6] << 8));
    float temp = (float)rawTemp / 100.0f;
    float hum = (float)rawHum / 100.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    out.temp = temp;
    out.hum = hum;
    if (len >= 8 && validBatt((int8_t)mfr[7])) out.batt = (int8_t)mfr[7];
    return true;
}

// Govee H510x/H5174/H5177/GV5179 — 3-byte combined at offset 4
// Manufacturer data 0x0001: [0-1]=company ID, [2-3]=header, [4-6]=combined, [7]=battery|error
static bool decodeGoveeV1(const uint8_t* mfr, uint8_t len, SensorReading& out) {
    if (len < 8 || (mfr[7] & 0x80)) return false;
    if (!decodeGoveeCombined(mfr, 4, out)) return false;
    out.batt = (int8_t)(mfr[7] & 0x7F);
    return true;
}

// Xiaomi LYWSD03MMC / CGG1 with PVVX custom firmware
// Service data UUID 0x181A: [0-5]=MAC, [6-7]=temp LE, [8-9]=hum LE, ..., [12]=battery
static bool decodePVVX(const uint8_t* svc, uint8_t len, SensorReading& out) {
    if (len < 13) return false;
    int16_t rawTemp = (int16_t)(svc[6] | (svc[7] << 8));
    uint16_t rawHum = (uint16_t)(svc[8] | (svc[9] << 8));
    float temp = (float)rawTemp / 100.0f;
    float hum = (float)rawHum / 100.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    out.temp = temp;
    out.hum = hum;
    if (validBatt((int8_t)svc[12])) out.batt = (int8_t)svc[12];
    return true;
}

// BTHome v2 — Service data UUID 0xFCD2, TLV objects
static bool decodeBTHome(const uint8_t* svc, uint8_t len, SensorReading& out) {
    if (len < 3) return false;
    uint8_t devInfo = svc[0];
    if ((devInfo & 0x01) != 0) return false;  // Encrypted
    bool gotTemp = false;
    uint8_t i = 1;
    while (i < len) {
        uint8_t objId = svc[i++];
        if (i >= len) break;
        switch (objId) {
            case 0x02: {
                if (i + 2 > len) return gotTemp;
                int16_t raw = (int16_t)(svc[i] | (svc[i+1] << 8));
                float t = (float)raw / 100.0f;
                if (validTemp(t)) { out.temp = t; gotTemp = true; }
                i += 2;
                break;
            }
            case 0x03: {
                if (i + 2 > len) return gotTemp;
                uint16_t raw = (uint16_t)(svc[i] | (svc[i+1] << 8));
                float h = (float)raw / 100.0f;
                if (validHum(h)) out.hum = h;
                i += 2;
                break;
            }
            case 0x01: {
                if (i + 1 > len) return gotTemp;
                if (validBatt((int8_t)svc[i])) out.batt = (int8_t)svc[i];
                i += 1;
                break;
            }
            default:
                return gotTemp;
        }
    }
    return gotTemp;
}

// SwitchBot Meter family — proprietary format, readings split across AD fields.
// Company ID 0x0969 is shared by every SwitchBot product with unrelated bytes
// at the same offsets, so the 0x0969 temp+hum decode is gated on a meter-type
// 0xFD3D service-data field. Each decoder fills what its field carries.
static bool isSwitchBotMeter(uint8_t devType) {
    return devType == 'T' ||   // Meter
           devType == 'i' ||   // Meter Plus
           devType == '4' ||   // Meter Pro
           devType == '5' ||   // Meter Pro CO2
           devType == 'w';     // Indoor/Outdoor Meter
}

// Device-type byte from the 0xFD3D service data (0 if absent) — gates the
// 0x0969 manufacturer decoder to meters only.
static uint8_t switchBotType(const uint8_t* adv, size_t totalLen) {
    size_t i = 0;
    while (i + 1 < totalLen) {
        uint8_t fieldLen = adv[i];
        if (fieldLen == 0 || i + fieldLen >= totalLen) break;
        if (adv[i + 1] == 0x16 && fieldLen >= 4) {
            uint16_t uuid = adv[i + 2] | (adv[i + 3] << 8);
            if (uuid == 0xFD3D) return adv[i + 4] & 0x7F;  // bit7 flags "new data"
        }
        i += fieldLen + 1;
    }
    return 0;
}

// A meter splits across two PDUs — 0xFD3D (type+battery) in the ADV_IND, 0x0969
// (temp+hum) in the SCAN_RSP — which NimBLE delivers as separate callbacks. So
// cache the meter type per MAC to carry the gate across the pair. Scan-callback
// only, no locking.
struct SbTypeCacheEntry { char mac[18]; uint8_t type; uint32_t seen; };
static SbTypeCacheEntry s_sbTypeCache[SB_TYPE_CACHE_SIZE];
static int s_sbTypeCacheCount = 0;

static uint8_t resolveSwitchBotType(const uint8_t* adv, size_t totalLen, const char* mac) {
    uint8_t t = switchBotType(adv, totalLen);
    if (mac == nullptr) return t;

    if (t != 0) {                                    // 0xFD3D present — cache it
        for (int i = 0; i < s_sbTypeCacheCount; i++) {
            if (strcasecmp(s_sbTypeCache[i].mac, mac) == 0) {
                s_sbTypeCache[i].type = t;
                s_sbTypeCache[i].seen = ++s_sbTypeTick;
                return t;
            }
        }
        int slot;
        if (s_sbTypeCacheCount < (int)SB_TYPE_CACHE_SIZE) {
            slot = s_sbTypeCacheCount++;
        } else {                                     // evict least-recently-seen
            slot = 0;
            for (int i = 1; i < s_sbTypeCacheCount; i++)
                if (s_sbTypeCache[i].seen < s_sbTypeCache[slot].seen) slot = i;
        }
        strncpy(s_sbTypeCache[slot].mac, mac, sizeof(s_sbTypeCache[slot].mac) - 1);
        s_sbTypeCache[slot].mac[sizeof(s_sbTypeCache[slot].mac) - 1] = '\0';
        s_sbTypeCache[slot].type = t;
        s_sbTypeCache[slot].seen = ++s_sbTypeTick;
        return t;
    }

    // Scan response (no 0xFD3D) — reuse the cached type
    for (int i = 0; i < s_sbTypeCacheCount; i++)
        if (strcasecmp(s_sbTypeCache[i].mac, mac) == 0)
            return s_sbTypeCache[i].type;
    return 0;
}

// Shared 3-byte temp+hum: [0] low nibble = 0.1°C (high nibble = alert flags,
// masked), [1] bits 0-6 = integer °C (bit7 = positive), [2] bits 0-6 = humidity.
// Integer-only range checks — runs in the scan callback and C3/C6 have no FPU.
static bool decodeSwitchBotTempHum(const uint8_t* d, SensorReading& out) {
    int frac    = d[0] & 0x0F;
    int tempInt = d[1] & 0x7F;
    int hum     = d[2] & 0x7F;
    bool positive = (d[1] & 0x80) != 0;
    int tenths  = tempInt * 10 + frac;                     // |temp| in 0.1°C
    if (frac > 9 || hum > 100 || tenths > (positive ? 800 : 400))
        return false;                                      // mirrors validTemp/validHum
    out.temp = positive ? (float)tenths / 10.0f : -(float)tenths / 10.0f;
    out.hum  = (float)hum;
    return true;
}

// Manufacturer data 0x0969, including the 2-byte company ID: temp+hum block at
// [10-12]. Only called once the 0xFD3D field confirmed a meter device type
static bool decodeSwitchBotMfr(const uint8_t* mfr, uint8_t len, SensorReading& out) {
    if (len < 13) return false;
    return decodeSwitchBotTempHum(mfr + 10, out);
}

// Service data UUID 0xFD3D, after the 2-byte UUID: [0]=device type,
// [2]=battery, temp+hum block at [3-5] on Meter/Meter Plus
static bool decodeSwitchBotSvc(const uint8_t* svc, uint8_t len, SensorReading& out) {
    if (len < 3) return false;
    uint8_t devType = svc[0] & 0x7F;
    if (!isSwitchBotMeter(devType)) return false;   // Bot, Curtain, ...
    bool gotData = false;
    if ((devType == 'T' || devType == 'i') && len >= 6)
        gotData = decodeSwitchBotTempHum(svc + 3, out);
    int8_t batt = (int8_t)(svc[2] & 0x7F);
    if (validBatt(batt)) { out.batt = batt; gotData = true; }
    return gotData;   // false when the frame yielded nothing usable
}

// ══════════════════════════════════════════════════════════════════════════════
// Decoder dispatch — try all decoders for a single AD field
// ══════════════════════════════════════════════════════════════════════════════

struct DecodeResult { bool decoded; const char* type; };

static DecodeResult tryDecode(uint8_t fieldType, const uint8_t* data, uint8_t len,
                              uint8_t sbType, SensorReading& out) {
    if (fieldType == 0xFF && len >= 2) {
        uint16_t cid = data[0] | (data[1] << 8);
        if (cid == 0xEC88) {
            if (len <= 7 && decodeGoveeV3(data, len, out))
                return {true, "Govee V3"};
            if (len > 7 && decodeGoveeV2(data, len, out))
                return {true, "Govee V2"};
        }
        if (cid == 0x0001 && len >= 8 && decodeGoveeV1(data, len, out))
            return {true, "Govee V1"};
        if (cid == 0x0969 && len >= 13 && isSwitchBotMeter(sbType) &&
            decodeSwitchBotMfr(data, len, out))
            return {true, "SwitchBot"};
    }
    if (fieldType == 0x16 && len >= 2) {
        uint16_t uuid = data[0] | (data[1] << 8);
        if (uuid == 0x181A && len >= 15 && decodePVVX(data + 2, len - 2, out))
            return {true, "PVVX"};
        if (uuid == 0xFCD2 && decodeBTHome(data + 2, len - 2, out))
            return {true, "BTHome v2"};
        if (uuid == 0xFD3D && decodeSwitchBotSvc(data + 2, len - 2, out))
            return {true, "SwitchBot"};
    }
    return {false, nullptr};
}

// ══════════════════════════════════════════════════════════════════════════════
// Discovery helpers
// ══════════════════════════════════════════════════════════════════════════════

// Walk every AD field, decoding sensor values into `out`. Returns the detected
// sensor type (string literal), or nullptr if no known sensor matched. Used by
// both the live feed and discovery, so the two paths can never disagree on what
// counts as a recognised sensor.
static const char* decodeAdvertisement(const uint8_t* adv, size_t totalLen,
                                       const char* mac, SensorReading& out) {
    const char* type = nullptr;
    const uint8_t sbType = resolveSwitchBotType(adv, totalLen, mac);
    size_t i = 0;
    while (i + 1 < totalLen) {
        uint8_t fieldLen = adv[i];
        if (fieldLen == 0 || i + fieldLen >= totalLen) break;
        DecodeResult r = tryDecode(adv[i + 1], &adv[i + 2], fieldLen - 1, sbType, out);
        if (r.decoded) type = r.type;
        i += fieldLen + 1;
    }
    return type;
}
