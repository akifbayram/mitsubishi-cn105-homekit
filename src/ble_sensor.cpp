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
// Advertisement callback
// ══════════════════════════════════════════════════════════════════════════════

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
    scan->setDuplicateFilter(true);
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
        cn105.sendRemoteTemperature(0);
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
