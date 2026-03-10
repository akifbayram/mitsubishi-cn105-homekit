# BLE Remote Temperature Sensor Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow users to pair a BLE temperature sensor and feed its readings to the heat pump via the CN105 0x07 remote temperature command, overriding the internal sensor for more accurate room temperature control.

**Architecture:** A `BLESensor` class uses NimBLE-Arduino for passive BLE advertisement scanning, decodes temperature from supported formats (Govee, BTHome v2, 0x181A), and feeds it to a new `CN105Controller::sendRemoteTemperature()` method every 20s. Configuration via web UI scan-and-select flow; settings persisted to NVS.

**Tech Stack:** NimBLE-Arduino (BLE), ESP-IDF UART (CN105), esp_http_server (WebSocket), Arduino/HomeSpan

**Spec:** `docs/plans/2026-03-10-ble-remote-temp-design.md`

---

## Chunk 1: Foundation (CN105 + Settings + Decoders)

### Task 1: Add NimBLE-Arduino dependency

**Files:**
- Modify: `platformio.ini:8-9`

- [ ] **Step 1: Add NimBLE-Arduino to lib_deps**

In `platformio.ini`, add `h2zero/NimBLE-Arduino@^2.2.1` to the `lib_deps` list:

```ini
lib_deps =
    HomeSpan/HomeSpan@2.1.5
    h2zero/NimBLE-Arduino@^2.2.1
```

- [ ] **Step 2: Verify build compiles**

Run: `pio run -e nanoc6`
Expected: BUILD SUCCESS (NimBLE downloads and links)

- [ ] **Step 3: Check flash usage**

Run: `pio run -e nanoc6 | grep -E "Flash:|RAM:"`
Expected: Flash usage increases by ~60-80KB but stays well under 1,984KB partition.

- [ ] **Step 4: Commit**

```bash
git add platformio.ini
git commit -m "feat(ble): add NimBLE-Arduino dependency"
```

---

### Task 2: Create BLE decoders

**Files:**
- Create: `include/ble_decoders.h`

These are pure functions with no dependencies beyond stdint — testable in isolation and reusable.

- [ ] **Step 1: Create ble_decoders.h with decoder structs and functions**

```cpp
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
```

- [ ] **Step 2: Verify build compiles**

Run: `pio run -e nanoc6`
Expected: BUILD SUCCESS (header included but not yet used — just validates syntax)

- [ ] **Step 3: Commit**

```bash
git add include/ble_decoders.h
git commit -m "feat(ble): add BLE advertisement decoder functions"
```

---

### Task 3: Add sendRemoteTemperature to CN105Controller

**Files:**
- Modify: `include/cn105_protocol.h:183-189` (add public method declaration)
- Modify: `src/cn105_protocol.cpp` (add implementation after sendSetPacket)

- [ ] **Step 1: Add method declaration to cn105_protocol.h**

Add after the `setWideVane` declaration (line 189):

```cpp
    /// Send remote temperature to heat pump (0x07 command).
    /// Positive value = use this temperature; 0 or negative = revert to internal sensor.
    void sendRemoteTemperature(float tempC);
```

- [ ] **Step 2: Add constant for remote temp command**

Add after `CN105_FLAG2_WVANE` (line 36):

```cpp
// ── Remote Temperature Command ──────────────────────────────────────────────
constexpr uint8_t CN105_CMD_REMOTE_TEMP = 0x07;
```

- [ ] **Step 3: Implement sendRemoteTemperature in cn105_protocol.cpp**

Add after `sendSetPacket()` (after line 478):

```cpp
void CN105Controller::sendRemoteTemperature(float tempC) {
    if (!_state.connected) return;

    // Don't send during an active poll cycle
    if (_cycleRunning) {
        LOG_DEBUG("[CN105] Deferring remote temp (cycle running)");
        return;
    }

    uint8_t pkt[22];
    memset(pkt, 0, sizeof(pkt));
    buildHeader(pkt, CN105_PKT_SET, CN105_DATA_LEN);

    pkt[5] = CN105_CMD_REMOTE_TEMP;

    if (tempC > 0.0f) {
        pkt[6] = 0x01;  // flag: providing remote temperature
        float rounded = roundf(tempC * 2.0f) / 2.0f;  // round to 0.5°C
        pkt[7] = (uint8_t)(rounded * 2.0f - 16.0f);   // legacy encoding
        pkt[8] = (uint8_t)(rounded * 2.0f + 128.0f);   // enhanced encoding
        LOG_INFO("[CN105] Sending remote temp: %.1f°C (legacy=0x%02X enhanced=0x%02X)",
                 rounded, pkt[7], pkt[8]);
    } else {
        pkt[6] = 0x00;  // flag: revert to internal sensor
        pkt[8] = 0x80;  // 128 = 0°C in enhanced encoding
        LOG_INFO("[CN105] Reverting to internal temperature sensor");
    }

    pkt[21] = calcChecksum(pkt, 21);

    if (currentLogLevel >= LOG_LEVEL_DEBUG) {
        DebugLog.printf("[CN105] TX REMOTE_TEMP (%d bytes): ", 22);
        logHex(pkt, 22);
    }

    uart_write_bytes(_uartNum, pkt, 22);
    uart_wait_tx_done(_uartNum, pdMS_TO_TICKS(200));
    _lastCycleEnd = millis() + CN105_DEFER_DELAY;
}
```

- [ ] **Step 4: Verify build compiles**

Run: `pio run -e nanoc6`
Expected: BUILD SUCCESS

- [ ] **Step 5: Commit**

```bash
git add include/cn105_protocol.h src/cn105_protocol.cpp
git commit -m "feat(cn105): add sendRemoteTemperature for 0x07 remote temp command"
```

---

### Task 4: Add BLE settings to DeviceSettings

**Files:**
- Modify: `include/settings.h:6-16` (add fields to DeviceSettings struct)
- Modify: `src/settings.cpp` (add NVS load/save)

- [ ] **Step 1: Add BLE fields to DeviceSettings struct**

Add after `vaneConfig` (line 15 in settings.h):

```cpp
    // BLE remote temperature sensor
    bool     bleEnabled = false;
    char     bleSensorAddr[18] = "";   // "AA:BB:CC:DD:EE:FF"
    char     bleSensorName[32] = "";   // Display name from scan
```

- [ ] **Step 2: Add NVS load in SettingsStore::begin()**

Add after the `vaneConfig` load (line 23 in settings.cpp):

```cpp
    _settings.bleEnabled = _prefs.getBool("bleOn", false);
    _prefs.getString("bleAddr", _settings.bleSensorAddr, sizeof(_settings.bleSensorAddr));
    _prefs.getString("bleName", _settings.bleSensorName, sizeof(_settings.bleSensorName));
```

- [ ] **Step 3: Add NVS save in SettingsStore::save()**

Add after the `vaneConfig` save (line 39 in settings.cpp):

```cpp
    _prefs.putBool("bleOn", _settings.bleEnabled);
    _prefs.putString("bleAddr", _settings.bleSensorAddr);
    _prefs.putString("bleName", _settings.bleSensorName);
```

- [ ] **Step 4: Verify build compiles**

Run: `pio run -e nanoc6`
Expected: BUILD SUCCESS

- [ ] **Step 5: Commit**

```bash
git add include/settings.h src/settings.cpp
git commit -m "feat(settings): add BLE sensor configuration to NVS"
```

---

## Chunk 2: BLESensor Class

### Task 5: Create BLESensor class

**Files:**
- Create: `include/ble_sensor.h`
- Create: `src/ble_sensor.cpp`

This is the core module. It manages NimBLE scanning, advertisement callbacks, format detection and decoding, and exposes current sensor readings.

- [ ] **Step 1: Create include/ble_sensor.h**

```cpp
#pragma once

#include <Arduino.h>
#include "ble_decoders.h"

// Maximum number of discovered sensors to hold during a scan
constexpr uint8_t BLE_MAX_DISCOVERED = 10;

// Scan timing
constexpr uint32_t BLE_SCAN_DURATION_S   = 5;     // seconds per scan window
constexpr uint32_t BLE_SCAN_INTERVAL_MS  = 10000; // ms between scans (monitoring mode)

// Stale timeout: revert to internal sensor if no update in this period
constexpr uint32_t BLE_STALE_TIMEOUT_MS  = 90000; // 90 seconds

// Keepalive: re-send remote temp to heat pump at this interval
constexpr uint32_t BLE_KEEPALIVE_MS      = 20000; // 20 seconds

// Detected BLE advertisement format
enum BLESensorFormat : uint8_t {
    BLE_FMT_UNKNOWN = 0,
    BLE_FMT_GOVEE_3BYTE,
    BLE_FMT_GOVEE_2BYTE,
    BLE_FMT_BTHOME,
    BLE_FMT_181A,
};

// Information about a discovered BLE sensor (from scan)
struct BLEDiscoveredSensor {
    char     name[32]  = "";
    char     addr[18]  = "";       // "AA:BB:CC:DD:EE:FF"
    int      rssi      = 0;
    BLESensorFormat format = BLE_FMT_UNKNOWN;
    float    temperature = 0.0f;   // decoded during discovery if possible
    bool     used       = false;   // slot in use
};

class BLESensor {
public:
    /// Initialize NimBLE stack (call once after WiFi is up)
    void begin();

    /// Call from main loop — manages scan cycles and keepalive
    void loop(class CN105Controller &cn105);

    /// Start a discovery scan (results available after BLE_SCAN_DURATION_S)
    void startDiscoveryScan();

    /// Check if discovery scan is still running
    bool isDiscoveryRunning() const { return _discoveryRunning; }

    /// Get discovery results (valid after scan completes)
    const BLEDiscoveredSensor* getDiscoveredSensors() const { return _discovered; }
    uint8_t getDiscoveredCount() const { return _discoveredCount; }

    /// Set the paired sensor by MAC address string ("AA:BB:CC:DD:EE:FF")
    void setSensor(const char *addr, const char *name);

    /// Clear the paired sensor and revert heat pump to internal sensor
    void clearSensor(class CN105Controller &cn105);

    /// Set remote temperature from external API (overrides BLE while fresh)
    void setApiTemperature(float tempC);

    /// Clear API temperature (resume BLE if configured)
    void clearApiTemperature();

    // ── Current readings ──────────────────────────────────────────────────────
    bool    isActive() const;          // sensor paired and receiving data
    bool    isStale() const;           // paired but no recent data
    float   getTemperature() const { return _temperature; }
    float   getHumidity() const { return _humidity; }
    int8_t  getBattery() const { return _battery; }
    int     getRSSI() const { return _rssi; }
    uint32_t getLastUpdate() const { return _lastUpdate; }
    const char* getSensorName() const { return _sensorName; }
    const char* getSensorAddr() const { return _sensorAddr; }
    bool    isEnabled() const { return _enabled; }
    bool    isApiMode() const { return _apiMode; }

    // Made accessible to scan callback (friend class pattern)
    float    _temperature    = 0.0f;
    float    _humidity       = -1.0f;
    int8_t   _battery        = -1;
    int      _rssi           = 0;
    uint32_t _lastUpdate     = 0;
    bool     _sentRevert     = false;
    bool     _scanning       = false;
    bool     _discoveryRunning = false;
    BLEDiscoveredSensor _discovered[BLE_MAX_DISCOVERED];
    uint8_t  _discoveredCount = 0;

    static const char* formatToStr(BLESensorFormat fmt);

private:
    bool     _initialized    = false;
    bool     _enabled        = false;   // sensor is paired and active
    bool     _apiMode        = false;   // temperature set via API, BLE paused

    // Paired sensor
    char     _sensorAddr[18] = "";
    char     _sensorName[32] = "";
    uint8_t  _targetAddr[6]  = {};      // parsed MAC bytes for fast comparison

    // Scan timing
    uint32_t _lastScanStart  = 0;

    // Keepalive
    uint32_t _lastKeepalive  = 0;

    // Internal
    void startMonitoringScan();
    void parseMacString(const char *str, uint8_t *out);
};

extern BLESensor bleSensor;
```

- [ ] **Step 2: Create src/ble_sensor.cpp**

```cpp
#include "ble_sensor.h"
#include "cn105_protocol.h"
#include "settings.h"
#include "logging.h"
#include <NimBLEDevice.h>

BLESensor bleSensor;

// ── Company IDs and UUIDs ───────────────────────────────────────────────────
static constexpr uint16_t GOVEE_COMPANY_ID = 0xEC88;
static const NimBLEUUID UUID_BTHOME("fcd2");
static const NimBLEUUID UUID_181A("181a");

// ══════════════════════════════════════════════════════════════════════════════
// Advertisement callback
// ══════════════════════════════════════════════════════════════════════════════

// Forward declarations for decode helpers
static BLESensorFormat detectFormat(const NimBLEAdvertisedDevice *device);
static BLEDecodeResult tryDecode(const NimBLEAdvertisedDevice *device);

class BLEScanCallback : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice *device) override {
        // ── Discovery mode: try all supported formats ────────────────────────
        if (bleSensor.isDiscoveryRunning()) {
            handleDiscovery(device);
            return;
        }

        // ── Monitoring mode: only process paired sensor ─────────────────────
        if (!bleSensor.isEnabled()) return;

        // Compare MAC address
        std::string advAddr = device->getAddress().toString();
        if (strcasecmp(advAddr.c_str(), bleSensor.getSensorAddr()) != 0) return;

        BLEDecodeResult result = tryDecode(device);
        if (result.valid) {
            bleSensor._temperature = result.temperature;
            bleSensor._humidity    = result.humidity;
            bleSensor._battery     = result.battery;
            bleSensor._rssi        = device->getRSSI();
            bleSensor._lastUpdate  = millis();
            bleSensor._sentRevert  = false;
            LOG_DEBUG("[BLE] Sensor update: %.1f°C  hum=%.1f%%  bat=%d%%  rssi=%d",
                      result.temperature, result.humidity, result.battery,
                      device->getRSSI());
        }
    }

    void onScanEnd(const NimBLEScanResults &results, int reason) override {
        bleSensor._scanning = false;
        if (bleSensor.isDiscoveryRunning()) {
            bleSensor._discoveryRunning = false;
            LOG_INFO("[BLE] Discovery scan complete: %d sensors found",
                     bleSensor._discoveredCount);
        } else {
            LOG_DEBUG("[BLE] Monitoring scan ended (%d devices seen)", results.getCount());
        }
    }

private:
    void handleDiscovery(const NimBLEAdvertisedDevice *device) {
        if (bleSensor._discoveredCount >= BLE_MAX_DISCOVERED) return;

        BLEDecodeResult result = tryDecode(device);
        BLESensorFormat fmt = detectFormat(device);

        // Only list devices we can decode (or have a known name pattern)
        if (!result.valid && fmt == BLE_FMT_UNKNOWN) return;

        // Check for duplicate MAC
        std::string addr = device->getAddress().toString();
        for (uint8_t i = 0; i < bleSensor._discoveredCount; i++) {
            if (strcasecmp(bleSensor._discovered[i].addr, addr.c_str()) == 0) return;
        }

        BLEDiscoveredSensor &s = bleSensor._discovered[bleSensor._discoveredCount];
        strncpy(s.addr, addr.c_str(), sizeof(s.addr) - 1);
        std::string name = device->getName();
        if (name.length() > 0) {
            strncpy(s.name, name.c_str(), sizeof(s.name) - 1);
        } else {
            strncpy(s.name, "Unknown", sizeof(s.name) - 1);
        }
        s.rssi = device->getRSSI();
        s.format = fmt;
        s.temperature = result.temperature;
        s.used = true;
        bleSensor._discoveredCount++;

        LOG_INFO("[BLE] Discovered: %s (%s) rssi=%d fmt=%s temp=%.1f",
                 s.name, s.addr, s.rssi, BLESensor::formatToStr(fmt),
                 result.temperature);
    }
};

static BLEScanCallback scanCallback;

// ══════════════════════════════════════════════════════════════════════════════
// Format detection and decoding (file-local helpers)
// ══════════════════════════════════════════════════════════════════════════════

static BLESensorFormat detectFormat(const NimBLEAdvertisedDevice *device) {
    // Check for Govee manufacturer data (company ID 0xEC88)
    if (device->haveManufacturerData()) {
        std::string mfgData = device->getManufacturerData();
        if (mfgData.size() >= 2) {
            uint16_t companyId = (uint8_t)mfgData[0] | ((uint8_t)mfgData[1] << 8);
            if (companyId == GOVEE_COMPANY_ID) {
                std::string name = device->getName();
                if (name.find("5074") != std::string::npos ||
                    name.find("5100") != std::string::npos ||
                    name.find("5104") != std::string::npos ||
                    name.find("5105") != std::string::npos ||
                    name.find("5179") != std::string::npos) {
                    return BLE_FMT_GOVEE_2BYTE;
                }
                return BLE_FMT_GOVEE_3BYTE;
            }
        }
    }
    // Check for BTHome v2 or 0x181A service data
    if (device->haveServiceData()) {
        if (device->getServiceDataUUID(0) == UUID_BTHOME) {
            return BLE_FMT_BTHOME;
        }
        if (device->getServiceDataUUID(0) == UUID_181A) {
            return BLE_FMT_181A;
        }
    }
    return BLE_FMT_UNKNOWN;
}

static BLEDecodeResult tryDecode(const NimBLEAdvertisedDevice *device) {
    BLEDecodeResult result;

    // Try Govee manufacturer data
    if (device->haveManufacturerData()) {
        std::string mfgData = device->getManufacturerData();
        if (mfgData.size() >= 2) {
            uint16_t companyId = (uint8_t)mfgData[0] | ((uint8_t)mfgData[1] << 8);
            if (companyId == GOVEE_COMPANY_ID && mfgData.size() >= 5) {
                const uint8_t *payload = (const uint8_t *)mfgData.data() + 2;
                size_t payloadLen = mfgData.size() - 2;

                // Try 2-byte format first (if name suggests it)
                std::string name = device->getName();
                if (name.find("5074") != std::string::npos ||
                    name.find("5100") != std::string::npos ||
                    name.find("5104") != std::string::npos ||
                    name.find("5105") != std::string::npos ||
                    name.find("5179") != std::string::npos) {
                    if (payloadLen >= 5) {
                        result = decodeGovee2Byte(payload + 1, payloadLen - 1);
                    }
                }

                // If 2-byte didn't work, try 3-byte
                if (!result.valid && payloadLen >= 4) {
                    result = decodeGovee3Byte(payload + 1, payloadLen - 1);
                    if (result.valid && payloadLen >= 5) {
                        result.battery = (int8_t)payload[4];
                    }
                }
                if (result.valid) return result;
            }
        }
    }

    // Try BTHome v2 service data
    if (device->haveServiceData()) {
        if (device->getServiceDataUUID(0) == UUID_BTHOME) {
            std::string svcData = device->getServiceData(0);
            result = decodeBTHome((const uint8_t *)svcData.data(), svcData.size());
            if (result.valid) return result;
        }

        // Try 0x181A Environmental Sensing
        if (device->getServiceDataUUID(0) == UUID_181A) {
            std::string svcData = device->getServiceData(0);
            result = decode181A((const uint8_t *)svcData.data(), svcData.size());
            if (result.valid) return result;
        }
    }

    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// Initialization
// ══════════════════════════════════════════════════════════════════════════════

void BLESensor::begin() {
    if (_initialized) return;

    LOG_INFO("[BLE] Initializing NimBLE stack");
    NimBLEDevice::init("");

    // Power saving: set low TX power (we only receive)
    NimBLEDevice::setPower(ESP_PWR_LVL_N12);

    _initialized = true;

    // Restore saved sensor config
    const DeviceSettings &cfg = settings.get();
    if (cfg.bleEnabled && strlen(cfg.bleSensorAddr) > 0) {
        setSensor(cfg.bleSensorAddr, cfg.bleSensorName);
        LOG_INFO("[BLE] Restored paired sensor: %s (%s)", _sensorName, _sensorAddr);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Main loop
// ══════════════════════════════════════════════════════════════════════════════

void BLESensor::loop(CN105Controller &cn105) {
    if (!_initialized) return;
    uint32_t now = millis();

    // ── Monitoring scan cycle ──────────────────────────────────────────────
    if (_enabled && !_discoveryRunning && !_scanning && !_apiMode) {
        if (now - _lastScanStart >= BLE_SCAN_INTERVAL_MS) {
            startMonitoringScan();
        }
    }

    // ── Keepalive: send remote temp to heat pump ────────────────────────────
    if ((_enabled || _apiMode) && !isStale()) {
        if (now - _lastKeepalive >= BLE_KEEPALIVE_MS) {
            cn105.sendRemoteTemperature(_temperature);
            _lastKeepalive = now;
        }
    }

    // ── Stale detection: revert to internal sensor ────────────────────────
    if ((_enabled || _apiMode) && isStale() && !_sentRevert) {
        LOG_WARN("[BLE] Sensor stale (no update for %lus), reverting to internal sensor",
                 (now - _lastUpdate) / 1000);
        cn105.sendRemoteTemperature(0);
        _sentRevert = true;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Scanning
// ══════════════════════════════════════════════════════════════════════════════

void BLESensor::startDiscoveryScan() {
    if (!_initialized) {
        begin();
    }

    LOG_INFO("[BLE] Starting discovery scan (%ds)", BLE_SCAN_DURATION_S);
    _discoveredCount = 0;
    memset(_discovered, 0, sizeof(_discovered));
    _discoveryRunning = true;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallback);
    scan->setActiveScan(true);   // needed for some Govee models (scan response)
    scan->setInterval(100);
    scan->setWindow(99);
    scan->setDuplicateFilter(false);
    scan->start(BLE_SCAN_DURATION_S, false);
    _scanning = true;
    _lastScanStart = millis();
}

void BLESensor::startMonitoringScan() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallback);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->setDuplicateFilter(true);  // only need one ad per device in monitoring
    scan->start(BLE_SCAN_DURATION_S, false);
    _scanning = true;
    _lastScanStart = millis();
    LOG_DEBUG("[BLE] Monitoring scan started");
}

// ══════════════════════════════════════════════════════════════════════════════
// Sensor management
// ══════════════════════════════════════════════════════════════════════════════

void BLESensor::setSensor(const char *addr, const char *name) {
    strncpy(_sensorAddr, addr, sizeof(_sensorAddr) - 1);
    _sensorAddr[sizeof(_sensorAddr) - 1] = '\0';
    strncpy(_sensorName, name, sizeof(_sensorName) - 1);
    _sensorName[sizeof(_sensorName) - 1] = '\0';
    parseMacString(addr, _targetAddr);
    _enabled = true;
    _sentRevert = false;
    _lastUpdate = 0;
    _apiMode = false;
    LOG_INFO("[BLE] Sensor paired: %s (%s)", _sensorName, _sensorAddr);
}

void BLESensor::clearSensor(CN105Controller &cn105) {
    if (_enabled || _apiMode) {
        cn105.sendRemoteTemperature(0);  // revert to internal sensor
    }
    _enabled = false;
    _apiMode = false;
    _sensorAddr[0] = '\0';
    _sensorName[0] = '\0';
    _temperature = 0;
    _humidity = -1;
    _battery = -1;
    _rssi = 0;
    _lastUpdate = 0;
    _sentRevert = false;
    LOG_INFO("[BLE] Sensor cleared, reverted to internal sensor");
}

void BLESensor::setApiTemperature(float tempC) {
    _temperature = tempC;
    _lastUpdate = millis();
    _apiMode = true;
    _sentRevert = false;
    LOG_INFO("[BLE] API remote temp set: %.1f°C", tempC);
}

void BLESensor::clearApiTemperature() {
    _apiMode = false;
    _lastUpdate = 0;
    _sentRevert = false;
    LOG_INFO("[BLE] API remote temp cleared");
}

// ══════════════════════════════════════════════════════════════════════════════
// Status
// ══════════════════════════════════════════════════════════════════════════════

bool BLESensor::isActive() const {
    return (_enabled || _apiMode) && _lastUpdate > 0 && !isStale();
}

bool BLESensor::isStale() const {
    if (_lastUpdate == 0) return true;
    return (millis() - _lastUpdate) > BLE_STALE_TIMEOUT_MS;
}

// ══════════════════════════════════════════════════════════════════════════════
// Helpers
// ══════════════════════════════════════════════════════════════════════════════

void BLESensor::parseMacString(const char *str, uint8_t *out) {
    unsigned int b[6] = {};
    sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
}

const char* BLESensor::formatToStr(BLESensorFormat fmt) {
    switch (fmt) {
        case BLE_FMT_GOVEE_3BYTE: return "Govee";
        case BLE_FMT_GOVEE_2BYTE: return "Govee";
        case BLE_FMT_BTHOME:      return "BTHome";
        case BLE_FMT_181A:        return "181A";
        default:                  return "Unknown";
    }
}
```

- [ ] **Step 3: Verify build compiles**

Run: `pio run -e nanoc6`
Expected: BUILD SUCCESS

- [ ] **Step 4: Commit**

```bash
git add include/ble_sensor.h src/ble_sensor.cpp
git commit -m "feat(ble): add BLESensor class with NimBLE scanning and decoder dispatch"
```

---

## Chunk 3: Web Server + Main Loop Integration

### Task 6: Add BLE WebSocket commands and state fields

**Files:**
- Modify: `include/web_server.h` (add include)
- Modify: `src/web_server.cpp` (add WS command handling, add BLE fields to pushState)

- [ ] **Step 1: Add BLE include to web_server.h**

At the top of `web_server.h`, add after `#include "settings.h"`:

```cpp
#include "ble_sensor.h"
```

- [ ] **Step 2: Add BLE WebSocket command handling in handleWsMessage**

In `src/web_server.cpp`, add `#include "ble_sensor.h"` at the top.

In `handleWsMessage()`, add new command branches before the final `else` (before line 812). Insert them after the `hkReset` handler:

```cpp
    } else if (strcmp(cmd, "bleScan") == 0) {
        LOG_INFO("[WebUI] BLE discovery scan requested");
        bleSensor.startDiscoveryScan();
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"bleScanStarted\"}");

    } else if (strcmp(cmd, "bleScanResults") == 0) {
        if (bleSensor.isDiscoveryRunning()) {
            sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"bleScanRunning\"}");
        } else {
            char rBuf[1024];
            int rn = snprintf(rBuf, sizeof(rBuf), "{\"type\":\"bleScanResults\",\"sensors\":[");
            for (uint8_t i = 0; i < bleSensor.getDiscoveredCount(); i++) {
                const BLEDiscoveredSensor &s = bleSensor.getDiscoveredSensors()[i];
                if (!s.used) continue;
                if (i > 0) rn += snprintf(rBuf + rn, sizeof(rBuf) - rn, ",");
                char escN[64];
                jsonEscape(s.name, escN, sizeof(escN));
                rn += snprintf(rBuf + rn, sizeof(rBuf) - rn,
                    "{\"name\":\"%s\",\"addr\":\"%s\",\"rssi\":%d,\"format\":\"%s\",\"temp\":%.1f}",
                    escN, s.addr, s.rssi, BLESensor::formatToStr(s.format), s.temperature);
                if (rn >= (int)sizeof(rBuf) - 50) break;
            }
            rn += snprintf(rBuf + rn, sizeof(rBuf) - rn, "]}");
            sendWsText(httpd_req_to_sockfd(req), rBuf);
        }

    } else if (strcmp(cmd, "bleSelect") == 0) {
        char addr[18] = {0};
        char name[32] = {0};
        if (jsonGetString(msg, "addr", addr, sizeof(addr))) {
            jsonGetString(msg, "name", name, sizeof(name));
            bleSensor.setSensor(addr, name);
            strncpy(settings.get().bleSensorAddr, addr, sizeof(settings.get().bleSensorAddr) - 1);
            strncpy(settings.get().bleSensorName, name, sizeof(settings.get().bleSensorName) - 1);
            settings.get().bleEnabled = true;
            settings.save();
            sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"bleSelected\"}");
            pushState();
        }

    } else if (strcmp(cmd, "bleDisable") == 0) {
        bleSensor.clearSensor(*_ctrl);
        settings.get().bleEnabled = false;
        settings.get().bleSensorAddr[0] = '\0';
        settings.get().bleSensorName[0] = '\0';
        settings.save();
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"bleDisabled\"}");
        pushState();
```

- [ ] **Step 3: Add remoteTemp handling to the existing "set" command**

Inside the `if (strcmp(cmd, "set") == 0)` block, add after the wide vane handling (after the `wideVane` jsonGetString block, around line 698):

```cpp
        float remoteTemp;
        if (jsonGetFloat(msg, "remoteTemp", &remoteTemp)) {
            if (remoteTemp > 0) {
                bleSensor.setApiTemperature(remoteTemp);
            } else {
                bleSensor.clearApiTemperature();
                _ctrl->sendRemoteTemperature(0);
            }
        }
```

- [ ] **Step 4: Add BLE fields to pushState()**

In `pushState()`, increase `buf` size from 800 to 1024:

```cpp
    char buf[1024];
```

Add BLE sensor status after the `coolThresh` fields and before the `logLevel` field:

```cpp
    // BLE remote sensor status
    if (bleSensor.isEnabled() || bleSensor.isApiMode()) {
        char escBN[64];
        jsonEscape(bleSensor.getSensorName(), escBN, sizeof(escBN));
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"bleSensor\":{\"name\":\"%s\",\"addr\":\"%s\""
            ",\"temp\":%.1f,\"hum\":%.1f,\"bat\":%d"
            ",\"rssi\":%d,\"active\":%s,\"stale\":%s,\"api\":%s"
            ",\"age\":%lu}",
            escBN, bleSensor.getSensorAddr(),
            bleSensor.getTemperature(), bleSensor.getHumidity(),
            bleSensor.getBattery(), bleSensor.getRSSI(),
            bleSensor.isActive() ? "true" : "false",
            bleSensor.isStale() ? "true" : "false",
            bleSensor.isApiMode() ? "true" : "false",
            bleSensor.getLastUpdate() > 0 ? (unsigned long)((millis() - bleSensor.getLastUpdate()) / 1000) : 0UL
        );
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"bleSensor\":null");
    }
```

- [ ] **Step 5: Verify build compiles**

Run: `pio run -e nanoc6`
Expected: BUILD SUCCESS

- [ ] **Step 6: Commit**

```bash
git add include/web_server.h src/web_server.cpp
git commit -m "feat(web): add BLE sensor WebSocket commands and state push"
```

---

### Task 7: Integrate BLE into main.cpp loop

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add BLE include**

Add after the other includes (line 12):

```cpp
#include "ble_sensor.h"
```

- [ ] **Step 2: Add BLE initialization after web UI start**

In `loop()`, add BLE initialization right after the web UI start block (after `webUIStarted = true;` on line 139):

```cpp
        // Initialize BLE sensor after WiFi is up
        if (settings.get().bleEnabled) {
            bleSensor.begin();
        }
```

- [ ] **Step 3: Add BLE loop call**

In `loop()`, add the BLE loop call after `webUI.loop();` (after line 149):

```cpp
        bleSensor.loop(cn105);
```

- [ ] **Step 4: Verify build compiles**

Run: `pio run -e nanoc6`
Expected: BUILD SUCCESS

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat(main): integrate BLE sensor into main loop"
```

---

## Chunk 4: Web UI

### Task 8: Add BLE sensor panel to web UI

**Files:**
- Modify: `web/index.html`

This task adds the Remote Temperature Sensor section to the Settings panel. The UI follows existing patterns: same styling, same WebSocket command dispatch, same state-driven rendering. All dynamic content is rendered using safe DOM methods (`textContent`, `createElement`) — not `innerHTML` with unsanitized strings.

- [ ] **Step 1: Add BLE sensor section HTML to the Settings panel**

In `web/index.html`, locate the Settings section (search for existing settings content like "Vane Configuration"). Add a new section after existing settings:

```html
<!-- Remote Temperature Sensor -->
<div class="setting-group" id="ble-section">
  <h3>Remote Temperature Sensor</h3>
  <div id="ble-status"></div>
  <div id="ble-controls" style="margin-top:8px">
    <button id="ble-scan-btn" onclick="bleScan()">Scan for Sensors</button>
    <button id="ble-disable-btn" onclick="bleDisable()" style="display:none;margin-left:8px" class="danger-btn">Disable</button>
  </div>
  <div id="ble-scan-results" style="margin-top:12px"></div>
</div>
```

- [ ] **Step 2: Add BLE JavaScript functions using safe DOM methods**

Add these functions to the existing `<script>` section:

```javascript
function bleScan() {
  var btn = document.getElementById('ble-scan-btn');
  btn.disabled = true;
  btn.textContent = 'Scanning...';
  document.getElementById('ble-scan-results').textContent = '';
  ws.send(JSON.stringify({cmd:'bleScan'}));
  setTimeout(function() {
    ws.send(JSON.stringify({cmd:'bleScanResults'}));
  }, 6000);
}

function bleSelect(addr, name) {
  ws.send(JSON.stringify({cmd:'bleSelect', addr: addr, name: name}));
  document.getElementById('ble-scan-results').textContent = '';
}

function bleDisable() {
  if (confirm('Disable remote temperature sensor? The heat pump will revert to its internal sensor.')) {
    ws.send(JSON.stringify({cmd:'bleDisable'}));
  }
}

function renderBleScanResults(data) {
  var btn = document.getElementById('ble-scan-btn');
  btn.disabled = false;
  btn.textContent = 'Scan for Sensors';
  var container = document.getElementById('ble-scan-results');
  // Clear previous results safely
  while (container.firstChild) container.removeChild(container.firstChild);

  if (!data.sensors || data.sensors.length === 0) {
    var msg = document.createElement('div');
    msg.style.cssText = 'color:#888;padding:8px';
    msg.textContent = 'No sensors found. Make sure your sensor is nearby and powered on.';
    container.appendChild(msg);
    return;
  }

  var list = document.createElement('div');
  list.style.fontSize = '13px';
  data.sensors.forEach(function(s) {
    var row = document.createElement('div');
    row.style.cssText = 'display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid var(--border,#eee)';

    var info = document.createElement('div');
    var nameEl = document.createElement('strong');
    nameEl.textContent = s.name;
    info.appendChild(nameEl);
    info.appendChild(document.createElement('br'));
    var detail = document.createElement('span');
    detail.style.cssText = 'color:#888;font-size:11px';
    var tempStr = s.temp ? ' (' + s.temp.toFixed(1) + '\u00B0)' : '';
    detail.textContent = s.addr + ' \u00B7 ' + s.format + tempStr + ' \u00B7 ' + s.rssi + 'dBm';
    info.appendChild(detail);
    row.appendChild(info);

    var selBtn = document.createElement('button');
    selBtn.textContent = 'Select';
    selBtn.style.padding = '4px 12px';
    (function(a, n) {
      selBtn.addEventListener('click', function() { bleSelect(a, n); });
    })(s.addr, s.name);
    row.appendChild(selBtn);

    list.appendChild(row);
  });
  container.appendChild(list);
}

function updateBleStatus(bleSensor) {
  var el = document.getElementById('ble-status');
  var disableBtn = document.getElementById('ble-disable-btn');
  // Clear previous content safely
  while (el.firstChild) el.removeChild(el.firstChild);

  if (!bleSensor) {
    el.textContent = 'No sensor configured';
    el.style.color = '#888';
    disableBtn.style.display = 'none';
    return;
  }
  el.style.color = '';
  disableBtn.style.display = 'inline-block';

  var wrap = document.createElement('div');
  wrap.style.fontSize = '13px';

  // Name + source
  var nameRow = document.createElement('div');
  var nameB = document.createElement('strong');
  nameB.textContent = bleSensor.name || 'Remote Sensor';
  nameRow.appendChild(nameB);
  var srcSpan = document.createElement('span');
  srcSpan.style.color = '#888';
  srcSpan.textContent = ' (' + (bleSensor.api ? 'API' : 'BLE') + ')';
  nameRow.appendChild(srcSpan);
  wrap.appendChild(nameRow);

  // Status
  var statusRow = document.createElement('div');
  var statusStr = bleSensor.active ? '\u{1F7E2} Active' : (bleSensor.stale ? '\u{1F534} Stale' : '\u23F3 Waiting...');
  statusRow.textContent = 'Status: ' + statusStr;
  wrap.appendChild(statusRow);

  // Readings
  if (bleSensor.active || bleSensor.stale) {
    var readRow = document.createElement('div');
    var parts = ['Temperature: ' + bleSensor.temp.toFixed(1) + '\u00B0'];
    if (bleSensor.hum >= 0) parts.push('Humidity: ' + bleSensor.hum.toFixed(0) + '%');
    if (bleSensor.bat >= 0) parts.push('Battery: ' + bleSensor.bat + '%');
    readRow.textContent = parts.join(' \u00B7 ');
    wrap.appendChild(readRow);

    if (!bleSensor.api) {
      var sigRow = document.createElement('div');
      sigRow.textContent = 'Signal: ' + bleSensor.rssi + 'dBm \u00B7 Updated ' + bleSensor.age + 's ago';
      wrap.appendChild(sigRow);
    }
  }
  el.appendChild(wrap);
}
```

- [ ] **Step 3: Integrate BLE state updates into existing WebSocket message handler**

In the existing WebSocket `onmessage` handler where state updates are processed (the function that handles `type === 'state'`), add:

```javascript
updateBleStatus(data.bleSensor);
```

Also add handling for BLE-specific message types in the message handler:

```javascript
if (data.type === 'bleScanResults') renderBleScanResults(data);
if (data.type === 'bleScanRunning') {
  setTimeout(function() { ws.send(JSON.stringify({cmd:'bleScanResults'})); }, 2000);
}
if (data.type === 'bleSelected' || data.type === 'bleDisabled') {
  // State will be refreshed by the next pushState
}
```

- [ ] **Step 4: Verify build compiles (triggers HTML embedding)**

Run: `pio run -e nanoc6`
Expected: BUILD SUCCESS (pre-build script regenerates `web_ui_html.h`)

- [ ] **Step 5: Commit**

```bash
git add web/index.html
git commit -m "feat(ui): add BLE remote temperature sensor panel to settings"
```

---

### Task 9: Final build verification

- [ ] **Step 1: Full clean build**

Run: `pio run -e nanoc6 --clean`
Expected: BUILD SUCCESS

- [ ] **Step 2: Check flash usage fits partition**

Run: `pio run -e nanoc6 -v 2>&1 | tail -5`
Expected: Flash usage well under 1,984KB

- [ ] **Step 3: Final commit if any remaining changes**

```bash
git status
# If any uncommitted changes:
git add -A
git commit -m "feat(ble): complete BLE remote temperature sensor support"
```

---

## Testing Checklist

Since this is an embedded project without a unit test framework, verification is manual:

- [ ] **Build verification**: `pio run -e nanoc6` completes without errors
- [ ] **Flash size**: firmware fits in 1,984KB partition
- [ ] **Boot without BLE**: device boots normally when `bleEnabled=false` (no NimBLE RAM overhead)
- [ ] **Discovery scan**: web UI "Scan for Sensors" finds nearby BLE sensors, shows name/MAC/format
- [ ] **Sensor select**: selecting a sensor saves to NVS, starts monitoring
- [ ] **Temperature readings**: sensor readings update in web UI status area
- [ ] **Keepalive**: remote temp sent to heat pump every 20s (verify in debug logs)
- [ ] **Stale handling**: removing sensor battery causes status to go stale after 90s, then reverts to internal
- [ ] **Disable**: "Disable" button clears sensor, sends revert command
- [ ] **Persist across reboot**: paired sensor restores after power cycle
- [ ] **API fallback**: `{"cmd":"set","remoteTemp":22.5}` via WebSocket overrides BLE
- [ ] **WiFi+BLE coexistence**: no crashes or connectivity issues under dual radio operation
- [ ] **Heap monitoring**: `ESP.getFreeHeap()` stays above 30KB during operation
