#include "ble_config.h"

#ifdef BLE_ENABLE

#include "ble_sensor.h"
#include "settings.h"
#include "logging.h"
#include "esp_utils.h"

#include "ble_decoders.h"

#include <cstring>
#include <cmath>
#include <atomic>

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

static std::atomic<bool> s_nimbleInitialized{false};
static std::atomic<bool> s_pendingInit{false};
static std::atomic<bool> s_pendingClear{false};   // Deferred clearRemoteTemperature
// Deferred scan stop+restart, drained in loop(). Set from the httpd task instead
// of calling stopScan()/startScan() directly: ble_gap_disc_cancel() can block on
// an HCI response and permanently stall the web server.
static std::atomic<bool> s_pendingRestart{false};
static std::atomic<bool> s_bleEnabled{false};      // Mirror of settings.bleEnabled

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

// Add a discovered device to the results array (deduplicate by MAC)
static void addDiscoveryResult(const char* addrLower, const char* name,
                               const char* type, int rssi, float temp, float hum) {
    // Convert to uppercase for display
    char addr[18];
    strncpy(addr, addrLower, 17);
    addr[17] = '\0';
    for (int j = 0; j < 17; j++) {
        if (addr[j] >= 'a' && addr[j] <= 'f') addr[j] -= 32;
    }

    // Already seen — refresh RSSI and the latest valid reading
    for (int j = 0; j < s_discoveryCount; j++) {
        if (strcmp(s_discovered[j].addr, addr) == 0) {
            s_discovered[j].rssi = rssi;
            if (!std::isnan(temp)) s_discovered[j].temperature = temp;
            if (!std::isnan(hum))  s_discovered[j].humidity    = hum;
            return;
        }
    }

    // List only once a temperature decodes — a battery-only frame can't identify it
    if (std::isnan(temp)) return;
    if (s_discoveryCount < BLE_MAX_DISCOVERED) {
        auto& d = s_discovered[s_discoveryCount];
        snprintf(d.addr, sizeof(d.addr), "%s", addr);
        snprintf(d.name, sizeof(d.name), "%s", name ? name : "");
        d.type = type;
        d.rssi = rssi;
        d.temperature = temp;
        d.humidity = hum;
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

        // Discovery mode: decode any sensor-type device for the results list so
        // the user can confirm the right device by its temperature/humidity
        if (s_discoveryMode) {
            SensorReading r;
            const char* type = decodeAdvertisement(disc->data, disc->length_data, addrStr, r);
            if (type) {
                char name[24];
                extractDeviceName(disc->data, disc->length_data, name, sizeof(name));
                addDiscoveryResult(addrStr, name, type, disc->rssi, r.temp, r.hum);
            }
        }

        // MAC filter: only process the configured target sensor
        if (!s_addrValid) return 0;
        if (strcasecmp(addrStr, s_targetLower) != 0) return 0;

        // Decode the live advertisement and publish the freshest reading
        SensorReading reading;
        const char* liveType = decodeAdvertisement(disc->data, disc->length_data, addrStr, reading);
        if (liveType) {
            taskENTER_CRITICAL(&s_mux);
            if (!std::isnan(reading.temp)) {
                s_temperature   = reading.temp;
                // Freshness follows the temperature: battery-only frames from
                // split-field sensors (SwitchBot Pro family) must not keep the
                // stale-revert watchdog from firing
                s_lastUpdate    = uptime_ms();
                s_staleReverted = false;
            }
            if (!std::isnan(reading.hum))  s_humidity = reading.hum;
            if (reading.batt >= 0)         s_battery  = reading.batt;
            s_sensorType = liveType;
            s_rssi       = disc->rssi;
            taskEXIT_CRITICAL(&s_mux);

            if (!s_typeLogged) {
                LOG_INFO("Detected sensor type: %s", liveType);
                s_typeLogged = true;
            }
        }

        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        // Scan ended (duration expired or was cancelled) — restart if needed
        s_scanning = false;
        if (s_nimbleInitialized.load() && s_bleEnabled.load() && (s_addrValid || s_discoveryMode)) {
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
    if (!s_nimbleInitialized.load()) return;
    if (s_scanning) return;

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.passive = 0;              // Active scan — required for sensors that put data in SCAN_RSP (e.g. Govee V2)
    params.filter_duplicates = 0;    // We want repeated advertisements
    params.itvl = 160;              // 100ms in 0.625ms units
    params.window = 144;            // 90ms in 0.625ms units (90% duty cycle)

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc == 0) {
        s_scanning = true;
    } else {
        LOG_WARN("ble_gap_disc failed: %d", rc);
    }
}

static void stopScan() {
    if (!s_nimbleInitialized.load()) return;
    if (!s_scanning) return;
    ble_gap_disc_cancel();
    s_scanning = false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════════

static bool initNimble() {
    if (s_nimbleInitialized.load()) return true;

    uint32_t heapBefore = esp_get_free_heap_size();

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        LOG_ERROR("nimble_port_init failed: %d", ret);
        return false;
    }

    nimble_port_freertos_init([](void *param) {
        nimble_port_run();
        nimble_port_freertos_deinit();
    });

    vTaskDelay(pdMS_TO_TICKS(200));
    s_nimbleInitialized.store(true);

    uint32_t heapAfter = esp_get_free_heap_size();
    LOG_INFO("NimBLE initialized. Heap: %u -> %u (-%u bytes)",
             heapBefore, heapAfter, heapBefore - heapAfter);
    if (heapAfter < 30000) {
        LOG_WARN("Low heap after BLE init: %u bytes remaining", heapAfter);
    }
    return true;
}

void BleSensor::begin() {
    const char* addr = settings.get().bleSensorAddr;
    if (strlen(addr) > 0 && parseMac(addr, s_targetAddr)) {
        s_addrValid = true;
        updateTargetLower();
        LOG_INFO("Target sensor: %s", addr);
    } else {
        LOG_INFO("No sensor MAC configured — scanning deferred");
    }

    s_bleEnabled.store(settings.get().bleEnabled);

    if (!s_bleEnabled.load()) {
        LOG_INFO("BLE disabled, skipping NimBLE init");
        return;
    }

    if (!initNimble()) return;

    if (s_addrValid) {
        startScan();
        LOG_INFO("Scanning started (auto-detect all sensor types)");
    }
}

void BleSensor::setBleEnabled(bool on) {
    s_bleEnabled.store(on);
    settings.get().bleEnabled = on;
    settings.save();

    if (on) {
        if (s_nimbleInitialized.load()) {
            s_pendingRestart.store(true);
        } else {
            // Defer NimBLE init to main loop (avoid blocking httpd task)
            s_pendingInit.store(true);
        }
        LOG_INFO("BLE enabled");
    } else {
        // s_bleEnabled is already false, so the restart handler stops without re-scanning
        s_pendingRestart.store(true);
        // Defer clearRemoteTemperature to loop() (needs cn105 reference)
        s_pendingClear.store(true);
        s_staleReverted = false;
        LOG_INFO("BLE disabled");
    }
}

bool BleSensor::isBleEnabled() {
    return s_bleEnabled.load();
}

void BleSensor::loop(CN105Controller &cn105) {
    // Handle deferred NimBLE init (requested from httpd task)
    if (s_pendingInit.exchange(false)) {
        if (!s_nimbleInitialized.load()) {
            if (initNimble() && s_addrValid) {
                startScan();
                LOG_INFO("Scanning started (deferred init)");
            }
        } else if (s_addrValid) {
            startScan();
        }
    }

    // Handle deferred clearRemoteTemperature (BLE was disabled from httpd task)
    if (s_pendingClear.exchange(false)) {
        if (cn105.isConnected()) {
            cn105.sendRemoteTemperature(0);
            LOG_INFO("Cleared remote temp — HP reverts to internal sensor");
        }
    }

    // Handle deferred scan restart (sensor address changed from httpd task)
    if (s_pendingRestart.exchange(false)) {
        stopScan();
        // Match the DISC_COMPLETE handler: discovery scans run without a
        // configured address (that's how new sensors get found).
        if (s_bleEnabled.load() && (s_addrValid || s_discoveryMode)) {
            startScan();
            LOG_INFO("Scan restarted for new sensor address");
        }
    }

    if (!s_bleEnabled.load()) return;

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
        s_pendingRestart.store(true);
        return;
    }

    if (parseMac(mac, s_targetAddr)) {
        s_addrValid = true;
        updateTargetLower();
        strncpy(settings.get().bleSensorAddr, mac, sizeof(settings.get().bleSensorAddr) - 1);
        settings.get().bleSensorAddr[sizeof(settings.get().bleSensorAddr) - 1] = '\0';
        settings.save();
        LOG_INFO("Sensor address set: %s", mac);
        s_pendingRestart.store(true);
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
    if (!s_bleEnabled.load() || !s_nimbleInitialized.load()) {
        LOG_WARN("BLE discovery rejected — BLE not enabled");
        return;
    }
    s_discoveryCount = 0;
    s_lastPushedCount = 0;
    s_discoveryMode = true;
    s_discoveryStart = uptime_ms();

    if (!s_scanning) {
        s_pendingRestart.store(true);
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
