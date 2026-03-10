#pragma once

#include <cstdint>
#include <cstring>

// Result from a BLE advertisement decode attempt
struct BLEDecodeResult {
    bool     valid       = false;
    float    temperature = 0.0f;
    float    humidity    = -1.0f;   // -1 = not available
    int8_t   battery     = -1;     // -1 = not available
};

// ── Govee 3-byte combined encoding ──────────────────────────────────────────
// Models: H5072, H5075, H5101, H5102, H5174, H5177
// Manufacturer data company ID: 0xEC88 (on wire: 88 EC)
// Temperature and humidity packed into 24-bit big-endian integer.
//
// data: pointer to the 3 payload bytes (after company ID and padding)
// dataLen: must be >= 3
inline BLEDecodeResult decodeGovee3Byte(const uint8_t *data, size_t dataLen) {
    BLEDecodeResult r;
    if (dataLen < 3) return r;

    int32_t raw = ((int32_t)data[0] << 16) | ((int32_t)data[1] << 8) | data[2];
    bool negative = (raw & 0x800000) != 0;
    if (negative) raw ^= 0x800000;

    r.temperature = (float)(raw / 1000) / 10.0f;
    r.humidity    = (float)(raw % 1000) / 10.0f;
    if (negative) r.temperature = -r.temperature;

    // Basic sanity check
    if (r.temperature >= -40.0f && r.temperature <= 60.0f &&
        r.humidity >= 0.0f && r.humidity <= 100.0f) {
        r.valid = true;
    }
    return r;
}

// ── Govee 2-byte separate encoding ──────────────────────────────────────────
// Models: H5074, H5100, H5104, H5105, H5179
// Temperature and humidity as separate 16-bit little-endian values.
//
// data: pointer to the payload bytes [temp_lo, temp_hi, hum_lo, hum_hi, battery]
// dataLen: must be >= 4 (5 if battery desired)
inline BLEDecodeResult decodeGovee2Byte(const uint8_t *data, size_t dataLen) {
    BLEDecodeResult r;
    if (dataLen < 4) return r;

    int16_t temp_raw = (int16_t)(data[0] | (data[1] << 8));
    uint16_t hum_raw = (uint16_t)(data[2] | (data[3] << 8));

    r.temperature = temp_raw / 100.0f;
    r.humidity    = hum_raw / 100.0f;

    if (dataLen >= 5) {
        r.battery = (int8_t)data[4];
    }

    if (r.temperature >= -40.0f && r.temperature <= 60.0f &&
        r.humidity >= 0.0f && r.humidity <= 100.0f) {
        r.valid = true;
    }
    return r;
}

// ── BTHome v2 decoding ──────────────────────────────────────────────────────
// Service data UUID: 0xFCD2
// Format: [device_info_byte] [obj_id] [data...] [obj_id] [data...] ...
// Object IDs: 0x02 = temperature (sint16, factor 0.01), 0x03 = humidity (uint16, factor 0.01)
//             0x01 = battery (uint8, %)
//
// data: pointer to service data payload (after UUID bytes)
// dataLen: length of payload
inline BLEDecodeResult decodeBTHome(const uint8_t *data, size_t dataLen) {
    BLEDecodeResult r;
    if (dataLen < 3) return r;  // need at least device info + 1 object

    size_t i = 1;  // skip device info byte
    while (i + 1 < dataLen) {
        uint8_t objId = data[i];
        i++;
        switch (objId) {
            case 0x01:  // battery (uint8)
                if (i < dataLen) {
                    r.battery = (int8_t)data[i];
                    i += 1;
                }
                break;
            case 0x02:  // temperature (sint16, 0.01 factor)
                if (i + 1 < dataLen) {
                    int16_t raw = (int16_t)(data[i] | (data[i + 1] << 8));
                    r.temperature = raw / 100.0f;
                    r.valid = true;
                    i += 2;
                }
                break;
            case 0x03:  // humidity (uint16, 0.01 factor)
                if (i + 1 < dataLen) {
                    uint16_t raw = (uint16_t)(data[i] | (data[i + 1] << 8));
                    r.humidity = raw / 100.0f;
                    i += 2;
                }
                break;
            case 0x2E:  // humidity (uint8, 1% factor — BTHome v2 alt)
                if (i < dataLen) {
                    r.humidity = (float)data[i];
                    i += 1;
                }
                break;
            case 0x45:  // temperature (sint16, 0.1 factor — BTHome v2 alt)
                if (i + 1 < dataLen) {
                    int16_t raw = (int16_t)(data[i] | (data[i + 1] << 8));
                    r.temperature = raw / 10.0f;
                    r.valid = true;
                    i += 2;
                }
                break;
            default:
                // Unknown object — can't determine size, stop parsing
                goto done;
        }
    }
done:
    if (r.valid && (r.temperature < -40.0f || r.temperature > 60.0f)) {
        r.valid = false;
    }
    return r;
}

// ── 0x181A Environmental Sensing service data ───────────────────────────────
// Used by pvvx custom firmware on Xiaomi LYWSD03MMC and other sensors.
// pvvx format (13 bytes after UUID):
//   bytes 0-5: MAC address
//   bytes 6-7: temperature as int16_t LE (0.01°C)
//   bytes 8-9: humidity as uint16_t LE (0.01%)
//   byte 10:   battery %
//   byte 11:   battery mV (high byte)
//   byte 12:   counter
//
// data: pointer to service data payload (after UUID bytes)
// dataLen: length of payload
inline BLEDecodeResult decode181A(const uint8_t *data, size_t dataLen) {
    BLEDecodeResult r;
    if (dataLen < 11) return r;  // need at least through battery byte

    int16_t temp_raw = (int16_t)(data[6] | (data[7] << 8));
    uint16_t hum_raw = (uint16_t)(data[8] | (data[9] << 8));

    r.temperature = temp_raw / 100.0f;
    r.humidity    = hum_raw / 100.0f;
    r.battery     = (int8_t)data[10];

    if (r.temperature >= -40.0f && r.temperature <= 60.0f &&
        r.humidity >= 0.0f && r.humidity <= 100.0f) {
        r.valid = true;
    }
    return r;
}
