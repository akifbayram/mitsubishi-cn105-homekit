#include "ble_config.h"

#ifdef BLE_ENABLE

#include "ble_sensor.h"
#include "settings.h"
#include "logging.h"
#include "esp_utils.h"

#include <cstring>
#include <cmath>

// Native NimBLE headers
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

static const char *TAG = "ble";

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

// ── Keepalive state ─────────────────────────────────────────────────────────
static uint32_t s_lastKeepalive = 0;

// ── Detected type ───────────────────────────────────────────────────────────
static const char* s_sensorType = nullptr;
static bool s_typeLogged = false;

// ── Discovery state ─────────────────────────────────────────────────────────
static BleDiscoveredDevice s_discovered[BLE_MAX_DISCOVERED];
static int      s_discoveryCount    = 0;
static bool     s_discoveryMode     = false;
static uint32_t s_discoveryStart    = 0;
static int      s_lastPushedCount   = 0;

// ── Validation ranges ───────────────────────────────────────────────────────
static inline bool validTemp(float t) { return t >= -40.0f && t <= 80.0f; }
static inline bool validHum(float h)  { return h >= 0.0f && h <= 100.0f; }
static inline bool validBatt(int8_t b){ return b >= 0 && b <= 100; }

// ── Spinlock helper ─────────────────────────────────────────────────────────
template<typename T>
static T readLocked(const T& var) {
    taskENTER_CRITICAL(&s_mux);
    T v = var;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

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

// ── Update lowercase MAC string for address comparison ──────────────────────
static void updateTargetLower() {
    snprintf(s_targetLower, sizeof(s_targetLower), "%02x:%02x:%02x:%02x:%02x:%02x",
             s_targetAddr[0], s_targetAddr[1], s_targetAddr[2],
             s_targetAddr[3], s_targetAddr[4], s_targetAddr[5]);
}

// ══════════════════════════════════════════════════════════════════════════════
// Decoders — all compiled in, dispatched at runtime
// ══════════════════════════════════════════════════════════════════════════════

// Shared Govee 3-byte combined temp+hum decoder (V3 offset 3, V1 offset 4)
static bool decodeGoveeCombined(const uint8_t* data, uint8_t offset) {
    int32_t val = ((int32_t)data[offset] << 16)
                | ((int32_t)data[offset+1] << 8)
                | data[offset+2];
    bool negative = (val & 0x800000);
    if (negative) val ^= 0x800000;
    float temp = (float)(val / 1000) / 10.0f;
    if (negative) temp = -temp;
    float hum = (float)(val % 1000) / 10.0f;
    if (!validTemp(temp) || !validHum(hum)) return false;
    s_temperature = temp;
    s_humidity = hum;
    return true;
}

// Govee H5072/H5075 — 3-byte combined temp+hum encoding
// Manufacturer data 0xEC88: [0-1]=company ID, [2]=padding, [3-5]=combined, [6]=battery|error
static bool decodeGoveeV3(const uint8_t* mfr, uint8_t len) {
    if (len < 7 || (mfr[6] & 0x80)) return false;
    if (!decodeGoveeCombined(mfr, 3)) return false;
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

// Govee H510x/H5174/H5177/GV5179 — 3-byte combined at offset 4
// Manufacturer data 0x0001: [0-1]=company ID, [2-3]=header, [4-6]=combined, [7]=battery|error
static bool decodeGoveeV1(const uint8_t* mfr, uint8_t len) {
    if (len < 8 || (mfr[7] & 0x80)) return false;
    if (!decodeGoveeCombined(mfr, 4)) return false;
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
// Decoder dispatch — try all decoders for a single AD field
// ══════════════════════════════════════════════════════════════════════════════

struct DecodeResult { bool decoded; const char* type; };

static DecodeResult tryDecode(uint8_t fieldType, const uint8_t* data, uint8_t len) {
    if (fieldType == 0xFF && len >= 2) {
        uint16_t cid = data[0] | (data[1] << 8);
        if (cid == 0xEC88) {
            if (len <= 7 && decodeGoveeV3(data, len))
                return {true, "Govee V3"};
            if (len > 7 && decodeGoveeV2(data, len))
                return {true, "Govee V2"};
        }
        if (cid == 0x0001 && len >= 8 && decodeGoveeV1(data, len))
            return {true, "Govee V1"};
    }
    if (fieldType == 0x16 && len >= 2) {
        uint16_t uuid = data[0] | (data[1] << 8);
        if (uuid == 0x181A && len >= 15 && decodePVVX(data + 2, len - 2))
            return {true, "PVVX"};
        if (uuid == 0xFCD2 && decodeBTHome(data + 2, len - 2))
            return {true, "BTHome v2"};
    }
    return {false, nullptr};
}

// ══════════════════════════════════════════════════════════════════════════════
// Discovery helpers
// ══════════════════════════════════════════════════════════════════════════════

// Identify sensor type from raw advertisement without decoding values
static const char* identifySensorType(const uint8_t* adv, size_t totalLen) {
    uint8_t i = 0;
    while (i + 1 < totalLen) {
        uint8_t fieldLen = adv[i];
        if (fieldLen == 0 || i + fieldLen >= totalLen) break;
        uint8_t fieldType = adv[i + 1];
        const uint8_t* fd = &adv[i + 2];
        uint8_t dl = fieldLen - 1;

        if (fieldType == 0xFF && dl >= 2) {
            uint16_t cid = fd[0] | (fd[1] << 8);
            if (cid == 0xEC88 && dl >= 7)
                return dl <= 7 ? "Govee V3" : "Govee V2";
            if (cid == 0x0001 && dl >= 8 && !(fd[7] & 0x80)) {
                int32_t val = ((int32_t)fd[4] << 16) | ((int32_t)fd[5] << 8) | fd[6];
                if (val & 0x800000) val ^= 0x800000;
                float t = (float)(val / 1000) / 10.0f;
                if (t >= -40.0f && t <= 80.0f) return "Govee V1";
            }
        }

        if (fieldType == 0x16 && dl >= 2) {
            uint16_t uuid = fd[0] | (fd[1] << 8);
            if (uuid == 0x181A && dl >= 15) return "PVVX";
            if (uuid == 0xFCD2 && dl >= 3) return "BTHome v2";
        }

        i += fieldLen + 1;
    }
    return nullptr;
}

// Add a discovered device to the results array (deduplicate by MAC)
static void addDiscoveryResult(const char* addrLower, const char* name,
                               const char* type, int rssi) {
    // Convert to uppercase for display
    char addr[18];
    strncpy(addr, addrLower, 17);
    addr[17] = '\0';
    for (int j = 0; j < 17; j++) {
        if (addr[j] >= 'a' && addr[j] <= 'f') addr[j] -= 32;
    }

    // Update RSSI if already seen
    for (int j = 0; j < s_discoveryCount; j++) {
        if (strcmp(s_discovered[j].addr, addr) == 0) {
            s_discovered[j].rssi = rssi;
            return;
        }
    }

    // Add new entry
    if (s_discoveryCount < BLE_MAX_DISCOVERED) {
        auto& d = s_discovered[s_discoveryCount];
        strncpy(d.addr, addr, sizeof(d.addr));
        strncpy(d.name, name ? name : "", sizeof(d.name) - 1);
        d.name[sizeof(d.name) - 1] = '\0';
        d.type = type;
        d.rssi = rssi;
        s_discoveryCount++;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Native NimBLE scan callback
// ══════════════════════════════════════════════════════════════════════════════

// Extract BLE device name from AD fields (AD type 0x09 = Complete Local Name,
// 0x08 = Shortened Local Name)
static void extractDeviceName(const uint8_t* data, uint8_t dataLen, char* out, size_t outLen) {
    out[0] = '\0';
    uint8_t i = 0;
    while (i + 1 < dataLen) {
        uint8_t fieldLen = data[i];
        if (fieldLen == 0 || i + fieldLen >= dataLen) break;
        uint8_t fieldType = data[i + 1];
        if (fieldType == 0x09 || fieldType == 0x08) {  // Complete or Shortened Local Name
            uint8_t nameLen = fieldLen - 1;
            if (nameLen >= outLen) nameLen = outLen - 1;
            memcpy(out, &data[i + 2], nameLen);
            out[nameLen] = '\0';
            return;
        }
        i += fieldLen + 1;
    }
}

static void startScan();

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *disc = &event->disc;

        // Extract address — NimBLE stores in LSB-first order
        const uint8_t *addr = disc->addr.val;
        char addrStr[18];
        snprintf(addrStr, sizeof(addrStr), "%02x:%02x:%02x:%02x:%02x:%02x",
            addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

        // Discovery mode: identify any sensor-type device
        if (s_discoveryMode) {
            const char* type = identifySensorType(disc->data, disc->length_data);
            if (type) {
                char name[24];
                extractDeviceName(disc->data, disc->length_data, name, sizeof(name));
                addDiscoveryResult(addrStr, name, type, disc->rssi);
            }
        }

        // MAC filter: only process the configured target sensor
        if (!s_addrValid) return 0;
        if (strcasecmp(addrStr, s_targetLower) != 0) return 0;

        // Iterate AD structures and try to decode
        const uint8_t* adv = disc->data;
        size_t totalLen = disc->length_data;
        uint8_t i = 0;
        while (i + 1 < totalLen) {
            uint8_t fieldLen = adv[i];
            if (fieldLen == 0 || i + fieldLen >= totalLen) break;

            taskENTER_CRITICAL(&s_mux);
            DecodeResult result = tryDecode(adv[i + 1], &adv[i + 2], fieldLen - 1);
            if (result.decoded) {
                s_sensorType = result.type;
                s_rssi = disc->rssi;
                s_lastUpdate = uptime_ms();
                s_staleReverted = false;
            }
            taskEXIT_CRITICAL(&s_mux);

            if (result.decoded && !s_typeLogged) {
                LOG_INFO("Detected sensor type: %s", result.type);
                s_typeLogged = true;
            }

            i += fieldLen + 1;
        }

        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        // Scan ended (duration expired or was cancelled) — restart if needed
        s_scanning = false;
        if (s_addrValid || s_discoveryMode) {
            startScan();
            LOG_DEBUG("Scan restarted");
        }
        return 0;
    }

    return 0;
}

// ══════════════════════════════════════════════════════════════════════════════
// Scan start/stop helpers
// ══════════════════════════════════════════════════════════════════════════════

static void startScan() {
    if (s_scanning) return;

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.passive = 1;              // Passive scan (no SCAN_REQ)
    params.filter_duplicates = 0;    // We want repeated advertisements
    params.itvl = 80;               // 50ms in 0.625ms units
    params.window = 48;             // 30ms in 0.625ms units

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc == 0) {
        s_scanning = true;
    } else {
        LOG_WARN("ble_gap_disc failed: %d", rc);
    }
}

static void stopScan() {
    if (!s_scanning) return;
    ble_gap_disc_cancel();
    s_scanning = false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════════

void BleSensor::begin() {
    const char* addr = settings.get().bleSensorAddr;
    if (strlen(addr) > 0 && parseMac(addr, s_targetAddr)) {
        s_addrValid = true;
        updateTargetLower();
        LOG_INFO("Target sensor: %s", addr);
    } else {
        LOG_INFO("No sensor MAC configured — scanning deferred");
    }

    uint32_t heapBefore = esp_get_free_heap_size();

    // Initialize NimBLE stack (controller + host)
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        LOG_ERROR("nimble_port_init failed: %d", ret);
        return;
    }

    // Start the NimBLE host task
    nimble_port_freertos_init([](void *param) {
        nimble_port_run();
        nimble_port_freertos_deinit();
    });

    // Give the host task time to sync with the controller
    vTaskDelay(pdMS_TO_TICKS(200));

    if (s_addrValid) {
        startScan();
        LOG_INFO("Scanning started (auto-detect all sensor types)");
    }

    uint32_t heapAfter = esp_get_free_heap_size();
    LOG_INFO("Init complete (NimBLE native). Heap: %u -> %u (-%u bytes)",
             heapBefore, heapAfter, heapBefore - heapAfter);
    if (heapAfter < 30000) {
        LOG_WARN("Low heap after BLE init: %u bytes remaining", heapAfter);
    }
}

void BleSensor::loop(CN105Controller &cn105) {
    uint32_t now = uptime_ms();
    float temp = readLocked(s_temperature);
    uint32_t lastUpd = readLocked(s_lastUpdate);

    uint32_t staleMs = (uint32_t)settings.get().bleStaleTimeoutS * 1000;
    bool active = lastUpd > 0 && (now - lastUpd) < staleMs;
    bool stale = lastUpd > 0 && !active;
    bool enabled = settings.get().bleFeedEnabled;

    if (active && enabled && cn105.isConnected() && !std::isnan(temp) && (now - s_lastKeepalive >= BLE_KEEPALIVE_MS)) {
        cn105.sendRemoteTemperature(temp);
        s_lastKeepalive = now;
        LOG_DEBUG("Keepalive sent: %.1f C", temp);
    }

    if (stale && enabled && cn105.isConnected() && !s_staleReverted) {
        cn105.sendRemoteTemperature(0);
        s_staleReverted = true;
        LOG_WARN("Sensor stale (%lus no data) — reverted to internal thermistor",
                 (unsigned long)((now - lastUpd) / 1000));
    }
}

float    BleSensor::temperature()  { return readLocked(s_temperature); }
float    BleSensor::humidity()     { return readLocked(s_humidity); }
int8_t   BleSensor::battery()      { return readLocked(s_battery); }
int      BleSensor::rssi()         { return readLocked(s_rssi); }

bool BleSensor::isActive() {
    uint32_t lu = readLocked(s_lastUpdate);
    uint32_t staleMs = (uint32_t)settings.get().bleStaleTimeoutS * 1000;
    return lu > 0 && (uptime_ms() - lu) < staleMs;
}

bool BleSensor::isStale() {
    uint32_t lu = readLocked(s_lastUpdate);
    uint32_t staleMs = (uint32_t)settings.get().bleStaleTimeoutS * 1000;
    return lu > 0 && (uptime_ms() - lu) >= staleMs;
}

uint32_t BleSensor::lastUpdateAge() {
    uint32_t lu = readLocked(s_lastUpdate);
    if (lu == 0) return UINT32_MAX;
    return uptime_ms() - lu;
}

bool BleSensor::isEnabled() {
    return settings.get().bleFeedEnabled;
}

void BleSensor::setEnabled(bool enabled) {
    settings.get().bleFeedEnabled = enabled;
    settings.save();
    LOG_INFO("Feed %s", enabled ? "enabled" : "disabled");
}

void BleSensor::setAddr(const char* mac) {
    if (!mac) return;

    stopScan();

    // Reset type detection for new sensor
    s_sensorType = nullptr;
    s_typeLogged = false;

    if (strlen(mac) == 0) {
        s_addrValid = false;
        memset(s_targetAddr, 0, 6);
        memset(s_targetLower, 0, sizeof(s_targetLower));
        strncpy(settings.get().bleSensorAddr, "", sizeof(settings.get().bleSensorAddr));
        settings.save();
        LOG_INFO("Sensor address cleared");
        return;
    }

    if (parseMac(mac, s_targetAddr)) {
        s_addrValid = true;
        updateTargetLower();
        strncpy(settings.get().bleSensorAddr, mac, sizeof(settings.get().bleSensorAddr) - 1);
        settings.get().bleSensorAddr[sizeof(settings.get().bleSensorAddr) - 1] = '\0';
        settings.save();
        LOG_INFO("Sensor address set: %s", mac);
        startScan();
    } else {
        LOG_WARN("Invalid MAC format: %s", mac);
    }
}

const char* BleSensor::getAddr() {
    return settings.get().bleSensorAddr;
}

const char* BleSensor::sensorType() {
    return s_sensorType;
}

void BleSensor::startDiscovery() {
    s_discoveryCount = 0;
    s_lastPushedCount = 0;
    s_discoveryMode = true;
    s_discoveryStart = uptime_ms();

    // Start scanning if not already running (e.g., no MAC configured)
    if (!s_scanning) {
        startScan();
    }

    LOG_INFO("Discovery scan started (%lums)", (unsigned long)BLE_DISCOVERY_MS);
}

bool BleSensor::isDiscovering() {
    return s_discoveryMode;
}

bool BleSensor::pollDiscoveryUpdate() {
    if (!s_discoveryMode) return false;
    if (s_discoveryCount > s_lastPushedCount) {
        s_lastPushedCount = s_discoveryCount;
        return true;
    }
    return false;
}

bool BleSensor::pollDiscoveryComplete() {
    if (s_discoveryMode && uptime_ms() - s_discoveryStart >= BLE_DISCOVERY_MS) {
        s_discoveryMode = false;

        // Stop scanning if no target configured
        if (!s_addrValid && s_scanning) {
            stopScan();
        }

        LOG_INFO("Discovery complete: %d sensor(s) found", s_discoveryCount);
        return true;
    }
    return false;
}

const BleDiscoveredDevice* BleSensor::discoveryResults(int& count) {
    count = s_discoveryCount;
    return s_discovered;
}

#endif // BLE_ENABLE
