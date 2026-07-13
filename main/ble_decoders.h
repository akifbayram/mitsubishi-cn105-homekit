#pragma once
// SwitchBot / Govee / PVVX / ATC1441 / BTHome BLE advertisement decoders.
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

// Govee H5072/H5075 — 3-byte combined temp+hum encoding. Real H5075 frames are
// 8 bytes incl company id (trailer byte, per govee-ble); older firmware sends 7.
// Manufacturer data 0xEC88: [0-1]=company ID, [2]=padding, [3-5]=combined, [6]=battery|error
static bool decodeGoveeV3(const uint8_t* mfr, uint8_t len, SensorReading& out) {
    if ((len != 7 && len != 8) || (mfr[6] & 0x80)) return false;
    if (!decodeGoveeCombined(mfr, 3, out)) return false;
    out.batt = (int8_t)(mfr[6] & 0x7F);
    return true;
}

// Govee H5074/H5051/H5052/H5071 — little-endian temp/hum. Frames are 9 bytes
// incl company id for H5074, 11 for H5051 (per govee-ble); the >=9 floor is
// what separates this family from the 7-8 byte V3 frames.
// Manufacturer data 0xEC88: [0-1]=company ID, [2]=reserved, [3-4]=temp LE, [5-6]=hum LE, [7]=battery
static bool decodeGoveeV2(const uint8_t* mfr, uint8_t len, SensorReading& out) {
    if (len < 9) return false;
    int16_t rawTemp = (int16_t)(mfr[3] | (mfr[4] << 8));
    uint16_t rawHum = (uint16_t)(mfr[5] | (mfr[6] << 8));
    float temp = (float)rawTemp / 100.0f;
    float hum = (float)rawHum / 100.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    out.temp = temp;
    out.hum = hum;
    if (validBatt((int8_t)mfr[7])) out.batt = (int8_t)mfr[7];
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

// Xiaomi LYWSD03MMC / CGG1 with PVVX firmware in its "custom" format (15 bytes,
// little-endian). Service data UUID 0x181A: [0-5]=MAC, [6-7]=temp LE (0.01 C),
// [8-9]=hum LE (0.01 %), [10-11]=battery mV, [12]=battery %, [13]=counter, [14]=flags
static bool decodePVVX(const uint8_t* svc, uint8_t len, SensorReading& out) {
    if (len < 15) return false;
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

// Xiaomi LYWSD03MMC with atc1441 firmware (or PVVX in "atc1441" format) —
// exactly 13 bytes, multi-byte fields BIG-endian (unlike PVVX custom).
// Service data UUID 0x181A: [0-5]=MAC, [6-7]=temp BE (0.1 C), [8]=humidity %,
// [9]=battery %, [10-11]=battery mV, [12]=frame counter
static bool decodeATC1441(const uint8_t* svc, uint8_t len, SensorReading& out) {
    if (len != 13) return false;
    float temp = (float)(int16_t)((svc[6] << 8) | svc[7]) / 10.0f;
    float hum  = (float)svc[8];
    if (!validTemp(temp) || !validHum(hum)) return false;
    out.temp = temp;
    out.hum  = hum;
    if (validBatt((int8_t)svc[9])) out.batt = (int8_t)svc[9];
    return true;
}

// Payload length of a BTHome v2 object, or -1 if unknown. Object ids are
// ordered ascending in the advertisement (per spec), so nothing above 0x45 —
// the last id we decode — can precede a temperature; the table stops there.
// Unknown ids stop the walk: the format has no per-object length byte, so a
// wrong guess would corrupt every field after it.
static int bthomeObjLen(uint8_t id) {
    if (id >= 0x0F && id <= 0x2D) return 1;   // binary sensors — all uint8
    switch (id) {
        case 0x00: case 0x01: case 0x09: case 0x2E: case 0x2F: case 0x3A:
            return 1;   // packet id, battery, count8, humidity8, moisture8, button
        case 0x02: case 0x03: case 0x06: case 0x07: case 0x08: case 0x0C:
        case 0x0D: case 0x0E: case 0x12: case 0x13: case 0x14: case 0x3C:
        case 0x3D: case 0x3F: case 0x40: case 0x41: case 0x43: case 0x44:
        case 0x45:
            return 2;   // 16-bit measurements/events
        case 0x04: case 0x05: case 0x0A: case 0x0B: case 0x42:
            return 3;   // 24-bit measurements
        case 0x3E:
            return 4;   // count32
        default:
            return -1;
    }
}

// BTHome v2 — Service data UUID 0xFCD2: device-info byte, then TLV objects.
// Decodes both temperature/humidity representations seen in the wild: 0x02/0x03
// (0.01 precision — PVVX, b-parasite) and 0x45/0x2E (0.1 C / 1 % — Shelly BLU H&T).
static bool decodeBTHome(const uint8_t* svc, uint8_t len, SensorReading& out) {
    if (len < 3) return false;
    if ((svc[0] & 0x01) != 0) return false;   // encrypted — can't decode
    if ((svc[0] >> 5) != 2) return false;     // not BTHome version 2
    SensorReading r;
    bool gotTemp = false;
    uint8_t i = 1;
    while (i < len) {
        uint8_t objId = svc[i++];
        int n = bthomeObjLen(objId);
        if (n < 0 || i + n > len) break;      // unknown id / truncated — stop
        switch (objId) {
            case 0x01:                        // battery, uint8 %
                if (validBatt((int8_t)svc[i])) r.batt = (int8_t)svc[i];
                break;
            case 0x02: {                      // temperature, sint16, 0.01 C
                float t = (float)(int16_t)(svc[i] | (svc[i+1] << 8)) / 100.0f;
                if (validTemp(t)) { r.temp = t; gotTemp = true; }
                break;
            }
            case 0x03: {                      // humidity, uint16, 0.01 %
                float h = (float)(uint16_t)(svc[i] | (svc[i+1] << 8)) / 100.0f;
                if (validHum(h)) r.hum = h;
                break;
            }
            case 0x2E:                        // humidity, uint8, 1 %
                if (validHum((float)svc[i])) r.hum = (float)svc[i];
                break;
            case 0x45: {                      // temperature, sint16, 0.1 C
                float t = (float)(int16_t)(svc[i] | (svc[i+1] << 8)) / 10.0f;
                if (validTemp(t)) { r.temp = t; gotTemp = true; }
                break;
            }
            default:                          // recognised but unused — skip
                break;
        }
        i += (uint8_t)n;
    }
    // A temperature is what identifies a usable sensor; publish only then, so a
    // rejected frame can't leak partial values into the caller's reading.
    if (!gotTemp) return false;
    out.temp = r.temp;
    if (!std::isnan(r.hum)) out.hum = r.hum;
    if (r.batt >= 0)        out.batt = r.batt;
    return true;
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
            // Each decoder gates on its own family's frame lengths (disjoint,
            // see the decoders), so trying both in order is unambiguous
            if (decodeGoveeV3(data, len, out)) return {true, "Govee V3"};
            if (decodeGoveeV2(data, len, out)) return {true, "Govee V2"};
        }
        if (cid == 0x0001 && len >= 8 && decodeGoveeV1(data, len, out))
            return {true, "Govee V1"};
        if (cid == 0x0969 && len >= 13 && isSwitchBotMeter(sbType) &&
            decodeSwitchBotMfr(data, len, out))
            return {true, "SwitchBot"};
    }
    if (fieldType == 0x16 && len >= 2) {
        uint16_t uuid = data[0] | (data[1] << 8);
        if (uuid == 0x181A) {
            // Same UUID, two incompatible layouts — each decoder gates on its
            // own frame length (atc1441 exactly 13, PVVX custom 15)
            if (decodeATC1441(data + 2, len - 2, out)) return {true, "ATC1441"};
            if (decodePVVX(data + 2, len - 2, out))    return {true, "PVVX"};
        }
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
