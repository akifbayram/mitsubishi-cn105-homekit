#include "ble_config.h"

#ifdef BLE_ENABLE

#include "ble_sensor.h"
#include "settings.h"
#include "logging.h"

#include <NimBLEDevice.h>
#include <cstring>
#include <cmath>

// ── Static state (thread-safe via spinlock) ─────────────────────────────────
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float    s_temperature = NAN;
static float    s_humidity    = NAN;
static int8_t   s_battery     = -1;
static int      s_rssi        = 0;
static uint32_t s_lastUpdate  = 0;

// ── Scanner state ───────────────────────────────────────────────────────────
static uint8_t  s_targetAddr[6] = {0};
static char     s_targetLower[18] = {0};  // lowercase "aa:bb:cc:dd:ee:ff"
static bool     s_addrValid     = false;
static bool     s_scanning      = false;
static bool     s_staleReverted = false;
static NimBLEScan* s_pScan      = nullptr;

// ── Keepalive state ─────────────────────────────────────────────────────────
static uint32_t s_lastKeepalive = 0;

// ── Detected type (logged once) ─────────────────────────────────────────────
static const char* s_detectedType = nullptr;

// ── Validation ranges ───────────────────────────────────────────────────────
static inline bool validTemp(float t) { return t >= -40.0f && t <= 80.0f; }
static inline bool validHum(float h)  { return h >= 0.0f && h <= 100.0f; }
static inline bool validBatt(int8_t b){ return b >= 0 && b <= 100; }

// ── MAC address parser: "AA:BB:CC:DD:EE:FF" → uint8_t[6] ───────────────────
static bool parseMac(const char* str, uint8_t out[6]) {
    if (!str || strlen(str) != 17) return false;
    unsigned int b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

// ── Update lowercase MAC string for NimBLE address comparison ───────────────
static void updateTargetLower() {
    snprintf(s_targetLower, sizeof(s_targetLower), "%02x:%02x:%02x:%02x:%02x:%02x",
             s_targetAddr[0], s_targetAddr[1], s_targetAddr[2],
             s_targetAddr[3], s_targetAddr[4], s_targetAddr[5]);
}

// ══════════════════════════════════════════════════════════════════════════════
// Decoders — all compiled in, dispatched at runtime
// ══════════════════════════════════════════════════════════════════════════════

// Govee H5072/H5075 — 3-byte combined temp+hum encoding
// Manufacturer data 0xEC88: [0-1]=company ID, [2]=padding, [3-5]=combined, [6]=battery|error
// Ref: HA govee-ble decode_temp_humid_battery_error() offset 1
static bool decodeGoveeV3(const uint8_t* mfr, uint8_t len) {
    if (len < 7) return false;
    if (mfr[6] & 0x80) return false;  // Error flag
    int32_t val = ((int32_t)mfr[3] << 16) | ((int32_t)mfr[4] << 8) | mfr[5];
    bool negative = false;
    if (val & 0x800000) { val = val ^ 0x800000; negative = true; }
    float temp = (float)(val / 1000) / 10.0f;
    if (negative) temp = -temp;
    float hum = (float)(val % 1000) / 10.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    s_temperature = temp;
    s_humidity = hum;
    s_battery = (int8_t)(mfr[6] & 0x7F);
    return true;
}

// Govee H5074/H5051/H5052/H5071 — little-endian temp/hum
// Manufacturer data 0xEC88: [0-1]=company ID, [2]=reserved, [3-4]=temp LE, [5-6]=hum LE, [7]=battery
static bool decodeGoveeV2(const uint8_t* mfr, uint8_t len) {
    if (len < 8) return false;
    int16_t rawTemp = (int16_t)(mfr[3] | (mfr[4] << 8));
    uint16_t rawHum = (uint16_t)(mfr[5] | (mfr[6] << 8));
    float temp = (float)rawTemp / 100.0f;
    float hum = (float)rawHum / 100.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    s_temperature = temp;
    s_humidity = hum;
    if (len >= 8 && validBatt((int8_t)mfr[7])) s_battery = (int8_t)mfr[7];
    return true;
}

// Govee H5100/H5101/H5102/H5103/H5104/H5105/H5108/H5110/H5174/H5177/GV5179
// 3-byte combined temp+hum encoding at offset 4 (after company ID + 2-byte header)
// Manufacturer data 0x0001: [0-1]=company ID, [2-3]=header, [4-6]=combined, [7]=battery|error
// Ref: HA govee-ble decode_temp_humid_battery_error() offset 2
static bool decodeGoveeV1(const uint8_t* mfr, uint8_t len) {
    if (len < 8) return false;
    if (mfr[7] & 0x80) return false;  // Error flag
    int32_t val = ((int32_t)mfr[4] << 16) | ((int32_t)mfr[5] << 8) | mfr[6];
    bool negative = false;
    if (val & 0x800000) { val = val ^ 0x800000; negative = true; }
    float temp = (float)(val / 1000) / 10.0f;
    if (negative) temp = -temp;
    float hum = (float)(val % 1000) / 10.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    s_temperature = temp;
    s_humidity = hum;
    s_battery = (int8_t)(mfr[7] & 0x7F);
    return true;
}

// Xiaomi LYWSD03MMC / CGG1 with PVVX custom firmware
// Service data UUID 0x181A: [0-5]=MAC, [6-7]=temp LE, [8-9]=hum LE, ..., [12]=battery
static bool decodePVVX(const uint8_t* svc, uint8_t len) {
    if (len < 13) return false;
    int16_t rawTemp = (int16_t)(svc[6] | (svc[7] << 8));
    uint16_t rawHum = (uint16_t)(svc[8] | (svc[9] << 8));
    float temp = (float)rawTemp / 100.0f;
    float hum = (float)rawHum / 100.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    s_temperature = temp;
    s_humidity = hum;
    if (validBatt((int8_t)svc[12])) s_battery = (int8_t)svc[12];
    return true;
}

// BTHome v2 — Service data UUID 0xFCD2, TLV objects
static bool decodeBTHome(const uint8_t* svc, uint8_t len) {
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
                if (validTemp(t)) { s_temperature = t; gotTemp = true; }
                i += 2;
                break;
            }
            case 0x03: {
                if (i + 2 > len) return gotTemp;
                uint16_t raw = (uint16_t)(svc[i] | (svc[i+1] << 8));
                float h = (float)raw / 100.0f;
                if (validHum(h)) s_humidity = h;
                i += 2;
                break;
            }
            case 0x01: {
                if (i + 1 > len) return gotTemp;
                if (validBatt((int8_t)svc[i])) s_battery = (int8_t)svc[i];
                i += 1;
                break;
            }
            default:
                return gotTemp;
        }
    }
    return gotTemp;
}

// ══════════════════════════════════════════════════════════════════════════════
// NimBLE Scan Callbacks
// ══════════════════════════════════════════════════════════════════════════════

class BLESensorScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* device) override {
        if (!s_addrValid) return;

        // MAC filter — only process the single configured sensor
        if (device->getAddress().toString() != s_targetLower) return;

        // Parse raw advertisement payload
        const std::vector<uint8_t>& payload = device->getPayload();
        size_t totalLen = payload.size();
        const uint8_t* adv = payload.data();

        uint8_t i = 0;
        while (i + 1 < totalLen) {
            uint8_t fieldLen = adv[i];
            if (fieldLen == 0 || i + fieldLen >= totalLen) break;
            uint8_t fieldType = adv[i + 1];
            const uint8_t *fieldData = &adv[i + 2];
            uint8_t dataLen = fieldLen - 1;

            bool decoded = false;

            // Manufacturer data (0xFF) — Govee sensors
            if (fieldType == 0xFF && dataLen >= 2) {
                uint16_t companyId = fieldData[0] | (fieldData[1] << 8);

                if (companyId == 0xEC88) {
                    taskENTER_CRITICAL(&s_mux);
                    if (dataLen <= 7) {
                        decoded = decodeGoveeV3(fieldData, dataLen);
                        if (decoded && !s_detectedType) s_detectedType = "Govee V3 (H5072/H5075)";
                    } else {
                        decoded = decodeGoveeV2(fieldData, dataLen);
                        if (decoded && !s_detectedType) s_detectedType = "Govee V2 (H5074/H5051/H5052/H5071)";
                    }
                    taskEXIT_CRITICAL(&s_mux);
                } else if (companyId == 0x0001 && dataLen >= 8) {
                    taskENTER_CRITICAL(&s_mux);
                    decoded = decodeGoveeV1(fieldData, dataLen);
                    if (decoded && !s_detectedType) s_detectedType = "Govee V1 (H510x/H5174/H5177/GV5179)";
                    taskEXIT_CRITICAL(&s_mux);
                }
            }

            // Service data (0x16) — PVVX / BTHome
            if (!decoded && fieldType == 0x16 && dataLen >= 2) {
                uint16_t uuid = fieldData[0] | (fieldData[1] << 8);

                if (uuid == 0x181A && dataLen >= 15) {
                    taskENTER_CRITICAL(&s_mux);
                    decoded = decodePVVX(fieldData + 2, dataLen - 2);
                    if (decoded && !s_detectedType) s_detectedType = "PVVX (LYWSD03MMC/CGG1)";
                    taskEXIT_CRITICAL(&s_mux);
                } else if (uuid == 0xFCD2) {
                    taskENTER_CRITICAL(&s_mux);
                    decoded = decodeBTHome(fieldData + 2, dataLen - 2);
                    if (decoded && !s_detectedType) s_detectedType = "BTHome v2";
                    taskEXIT_CRITICAL(&s_mux);
                }
            }

            if (decoded) {
                taskENTER_CRITICAL(&s_mux);
                s_rssi = device->getRSSI();
                s_lastUpdate = millis();
                s_staleReverted = false;
                taskEXIT_CRITICAL(&s_mux);

                if (s_detectedType) {
                    LOG_INFO("[BLE] Detected sensor type: %s", s_detectedType);
                    s_detectedType = nullptr;  // Log once
                }
            }

            i += fieldLen + 1;
        }
    }

    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        if (s_addrValid && s_pScan) {
            s_pScan->start(0);
            LOG_DEBUG("[BLE] Scan restarted");
        }
    }
};

static BLESensorScanCB scanCallbacks;

// ══════════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════════

void BleSensor::begin() {
    const char* addr = settings.get().bleSensorAddr;
    if (strlen(addr) > 0 && parseMac(addr, s_targetAddr)) {
        s_addrValid = true;
        updateTargetLower();
        LOG_INFO("[BLE] Target sensor: %s", addr);
    } else {
        LOG_INFO("[BLE] No sensor MAC configured — scanning deferred");
    }

    uint32_t heapBefore = esp_get_free_heap_size();

    NimBLEDevice::init("");
    s_pScan = NimBLEDevice::getScan();
    s_pScan->setScanCallbacks(&scanCallbacks, true);  // true = want duplicates
    s_pScan->setActiveScan(true);
    s_pScan->setInterval(50);   // 50ms
    s_pScan->setWindow(30);     // 30ms
    s_pScan->setDuplicateFilter(false);

    if (s_addrValid) {
        s_pScan->start(0);  // 0 = scan forever
        s_scanning = true;
        LOG_INFO("[BLE] Scanning started (auto-detect all sensor types)");
    }

    uint32_t heapAfter = esp_get_free_heap_size();
    LOG_INFO("[BLE] Init complete (NimBLE). Heap: %u -> %u (-%u bytes)",
             heapBefore, heapAfter, heapBefore - heapAfter);
    if (heapAfter < 30000) {
        LOG_WARN("[BLE] Low heap after BLE init: %u bytes remaining", heapAfter);
    }
}

void BleSensor::loop(CN105Controller &cn105) {
    uint32_t now = millis();

    float temp;
    uint32_t lastUpd;
    taskENTER_CRITICAL(&s_mux);
    temp = s_temperature;
    lastUpd = s_lastUpdate;
    taskEXIT_CRITICAL(&s_mux);

    uint32_t staleMs = (uint32_t)settings.get().bleStaleTimeoutS * 1000;
    bool active = lastUpd > 0 && (now - lastUpd) < staleMs;
    bool stale = lastUpd > 0 && !active;
    bool enabled = settings.get().bleFeedEnabled;

    if (active && enabled && !isnan(temp) && (now - s_lastKeepalive >= BLE_KEEPALIVE_MS)) {
        cn105.sendRemoteTemperature(temp);
        s_lastKeepalive = now;
        LOG_DEBUG("[BLE] Keepalive sent: %.1f°C", temp);
    }

    if (stale && enabled && !s_staleReverted) {
        cn105.sendRemoteTemperature(0);
        s_staleReverted = true;
        LOG_WARN("[BLE] Sensor stale (%lus no data) — reverted to internal thermistor",
                 (unsigned long)((now - lastUpd) / 1000));
    }
}

float BleSensor::temperature() {
    taskENTER_CRITICAL(&s_mux);
    float v = s_temperature;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

float BleSensor::humidity() {
    taskENTER_CRITICAL(&s_mux);
    float v = s_humidity;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

int8_t BleSensor::battery() {
    taskENTER_CRITICAL(&s_mux);
    int8_t v = s_battery;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

int BleSensor::rssi() {
    taskENTER_CRITICAL(&s_mux);
    int v = s_rssi;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

bool BleSensor::isActive() {
    taskENTER_CRITICAL(&s_mux);
    uint32_t lu = s_lastUpdate;
    taskEXIT_CRITICAL(&s_mux);
    uint32_t staleMs = (uint32_t)settings.get().bleStaleTimeoutS * 1000;
    return lu > 0 && (millis() - lu) < staleMs;
}

bool BleSensor::isStale() {
    taskENTER_CRITICAL(&s_mux);
    uint32_t lu = s_lastUpdate;
    taskEXIT_CRITICAL(&s_mux);
    uint32_t staleMs = (uint32_t)settings.get().bleStaleTimeoutS * 1000;
    return lu > 0 && (millis() - lu) >= staleMs;
}

uint32_t BleSensor::lastUpdateAge() {
    taskENTER_CRITICAL(&s_mux);
    uint32_t lu = s_lastUpdate;
    taskEXIT_CRITICAL(&s_mux);
    if (lu == 0) return UINT32_MAX;
    return millis() - lu;
}

bool BleSensor::isEnabled() {
    return settings.get().bleFeedEnabled;
}

void BleSensor::setEnabled(bool enabled) {
    settings.get().bleFeedEnabled = enabled;
    settings.save();
    LOG_INFO("[BLE] Feed %s", enabled ? "enabled" : "disabled");
}

void BleSensor::setAddr(const char* mac) {
    if (!mac) return;

    if (s_scanning && s_pScan) {
        s_pScan->stop();
        s_scanning = false;
    }

    if (strlen(mac) == 0) {
        s_addrValid = false;
        memset(s_targetAddr, 0, 6);
        memset(s_targetLower, 0, sizeof(s_targetLower));
        strncpy(settings.get().bleSensorAddr, "", sizeof(settings.get().bleSensorAddr));
        settings.save();
        LOG_INFO("[BLE] Sensor address cleared");
        return;
    }

    if (parseMac(mac, s_targetAddr)) {
        s_addrValid = true;
        updateTargetLower();
        strncpy(settings.get().bleSensorAddr, mac, sizeof(settings.get().bleSensorAddr) - 1);
        settings.get().bleSensorAddr[sizeof(settings.get().bleSensorAddr) - 1] = '\0';
        settings.save();
        LOG_INFO("[BLE] Sensor address set: %s", mac);
        if (s_pScan) {
            s_pScan->start(0);
            s_scanning = true;
        }
    } else {
        LOG_WARN("[BLE] Invalid MAC format: %s", mac);
    }
}

const char* BleSensor::getAddr() {
    return settings.get().bleSensorAddr;
}

#endif // BLE_ENABLE
