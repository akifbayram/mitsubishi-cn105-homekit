#include "ble_config.h"

#ifdef BLE_ENABLE

#include "ble_sensor.h"
#include "settings.h"
#include "sl2_proto.h"
#include "logging.h"
#include "esp_utils.h"
#include "link_sensor.h"

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

// ══════════════════════════════════════════════════════════════════════════════
// State
//
// Three tasks touch this module: the NimBLE host task (scan callback), the
// httpd task (web UI setters), and the main task (loop()). Shared multi-byte
// state lives under s_mux; cross-task flags are std::atomic; everything else is
// main-task-only and marked as such.
// ══════════════════════════════════════════════════════════════════════════════

// ── Latest reading (guarded by s_mux) ───────────────────────────────────────
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float    s_temperature = NAN;
static float    s_humidity    = NAN;
static int8_t   s_battery     = -1;
static int      s_rssi        = 0;
static uint32_t s_lastUpdate  = 0;

// ── Scan target (guarded by s_mux — setAddr rewrites it from the httpd task
//    while the scan callback compares against it) ────────────────────────────
static char     s_targetMac[18] = {0};   // "AA:BB:CC:DD:EE:FF"; compared case-insensitively
static bool     s_addrValid     = false;

// ── Scan duty profiles ──────────────────────────────────────────────────────
// SEARCH runs a 90 % window to find the sensor fast; TRACK drops to 30 % once
// readings flow — advertisements arrive every ~2 s, so a 60 ms window every
// 200 ms still catches one within a few seconds while easing WiFi coexistence
// and power. Both stay ACTIVE scans: Govee V2 readings ride in the SCAN_RSP.
enum class ScanProfile : uint8_t { SEARCH, TRACK };
struct ScanParams { uint16_t itvl, window; };            // units of 0.625 ms
static constexpr ScanParams SCAN_PARAMS[] = {
    {160, 144},   // SEARCH: 100 ms interval / 90 ms window
    {320,  96},   // TRACK:  200 ms interval / 60 ms window
};
static std::atomic<ScanProfile> s_scanProfile{ScanProfile::SEARCH};

static std::atomic<bool> s_scanning{false};
static std::atomic<bool> s_staleReverted{false};

static std::atomic<bool> s_nimbleInitialized{false};
static std::atomic<bool> s_pendingSync{false};    // Enable toggle changed — (re)sync NimBLE state
static std::atomic<bool> s_pendingClear{false};   // Deferred clearRemoteTemperature
// Deferred scan stop+restart, drained in loop(). Set from the httpd task instead
// of calling stopScan()/startScan() directly: ble_gap_disc_cancel() can block on
// an HCI response and permanently stall the web server.
static std::atomic<bool> s_pendingRestart{false};
static std::atomic<bool> s_bleEnabled{false};      // Mirror of settings.bleEnabled

// ── Keepalive state (main task only) ────────────────────────────────────────
static uint32_t s_lastKeepalive = 0;
static float    s_lastSentTemp  = NAN;   // last value sent to the HP
static bool     s_prevFeed      = false; // feed-toggle edge detection

// ── Detected type (logged once per configured sensor, on nullptr → value) ───
static std::atomic<const char*> s_sensorType{nullptr};

// ── Discovery state ─────────────────────────────────────────────────────────
// Results are written from the NimBLE host task and read from the main task —
// array, count, truncated flag, and pushed-count all live under s_mux.
static BleDiscoveredDevice s_discovered[BLE_MAX_DISCOVERED];
static int      s_discoveryCount     = 0;
static bool     s_discoveryTruncated = false;
static int      s_lastPushedCount    = 0;
static std::atomic<bool>     s_discoveryMode{false};
static std::atomic<uint32_t> s_discoveryStart{0};

// ── Spinlock helper ─────────────────────────────────────────────────────────
template<typename T>
static T readLocked(const T& var) {
    taskENTER_CRITICAL(&s_mux);
    T v = var;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

static bool targetConfigured() {
    return readLocked(s_addrValid);
}

// A scan should be running whenever there's a target to track or a discovery
// window open (callers also check NimBLE is up and BLE enabled)
static bool wantScan() {
    return targetConfigured() || s_discoveryMode.load();
}

// ── MAC format validator: "AA:BB:CC:DD:EE:FF" (any case) ───────────────────
static bool validMac(const char* str) {
    if (!str || strlen(str) != 17) return false;
    unsigned int b[6];
    return sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
                  &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6;
}

// Record a discovered device (deduplicated by MAC). Runs in the NimBLE scan
// callback; the results array is shared with the main task, so the search and
// insert happen under s_mux (string prep stays outside the lock).
static void addDiscoveryResult(const char* addrLower, const char* name,
                               const char* type, int rssi, float temp, float hum) {
    BleDiscoveredDevice d;
    snprintf(d.addr, sizeof(d.addr), "%s", addrLower);
    for (int j = 0; j < 17 && d.addr[j]; j++) {
        if (d.addr[j] >= 'a' && d.addr[j] <= 'f') d.addr[j] -= 32;  // uppercase for display
    }
    snprintf(d.name, sizeof(d.name), "%s", name ? name : "");
    d.type        = type;
    d.rssi        = rssi;
    d.temperature = temp;
    d.humidity    = hum;

    taskENTER_CRITICAL(&s_mux);
    // Already seen — refresh RSSI and the latest valid reading
    for (int j = 0; j < s_discoveryCount; j++) {
        if (strcmp(s_discovered[j].addr, d.addr) == 0) {
            s_discovered[j].rssi = rssi;
            if (!std::isnan(temp)) s_discovered[j].temperature = temp;
            if (!std::isnan(hum))  s_discovered[j].humidity    = hum;
            taskEXIT_CRITICAL(&s_mux);
            return;
        }
    }
    // List only once a temperature decodes — a battery-only frame can't identify it
    if (!std::isnan(temp)) {
        if (s_discoveryCount < BLE_MAX_DISCOVERED) s_discovered[s_discoveryCount++] = d;
        else s_discoveryTruncated = true;
    }
    taskEXIT_CRITICAL(&s_mux);
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
        if (s_discoveryMode.load()) {
            SensorReading r;
            const char* type = decodeAdvertisement(disc->data, disc->length_data, addrStr, r);
            if (type) {
                char name[24];
                extractDeviceName(disc->data, disc->length_data, name, sizeof(name));
                addDiscoveryResult(addrStr, name, type, disc->rssi, r.temp, r.hum);
            }
        }

        // MAC filter: only process the configured target sensor. Snapshot the
        // target under the lock — setAddr rewrites it from the httpd task.
        bool addrValid;
        char target[18];
        taskENTER_CRITICAL(&s_mux);
        addrValid = s_addrValid;
        memcpy(target, s_targetMac, sizeof(target));
        taskEXIT_CRITICAL(&s_mux);
        if (!addrValid || strcasecmp(addrStr, target) != 0) return 0;

        // Decode the live advertisement and publish the freshest reading
        SensorReading reading;
        const char* liveType = decodeAdvertisement(disc->data, disc->length_data, addrStr, reading);
        if (liveType) {
            bool gotTemp = !std::isnan(reading.temp);
            taskENTER_CRITICAL(&s_mux);
            if (gotTemp) {
                s_temperature = reading.temp;
                // Freshness follows the temperature: battery-only frames from
                // split-field sensors (SwitchBot Pro family) must not keep the
                // stale-revert watchdog from firing
                s_lastUpdate  = uptime_ms();
            }
            if (!std::isnan(reading.hum))  s_humidity = reading.hum;
            if (reading.batt >= 0)         s_battery  = reading.batt;
            s_rssi = disc->rssi;
            taskEXIT_CRITICAL(&s_mux);

            if (gotTemp) s_staleReverted.store(false);
            if (s_sensorType.exchange(liveType) == nullptr) {
                LOG_INFO("Detected sensor type: %s", liveType);
            }
        }

        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        // Scan ended (duration expired or was cancelled) — restart if needed
        s_scanning.store(false);
        if (s_nimbleInitialized.load() && s_bleEnabled.load() && wantScan()) {
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
    if (s_scanning.load()) return;

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.passive = 0;              // Active scan — required for sensors that put data in SCAN_RSP (e.g. Govee V2)
    params.filter_duplicates = 0;    // We want repeated advertisements
    const ScanParams& sp = SCAN_PARAMS[(uint8_t)s_scanProfile.load()];
    params.itvl   = sp.itvl;
    params.window = sp.window;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc == 0) {
        s_scanning.store(true);
    } else {
        LOG_WARN("ble_gap_disc failed: %d", rc);
    }
}

static void stopScan() {
    if (!s_nimbleInitialized.load()) return;
    if (!s_scanning.load()) return;
    ble_gap_disc_cancel();
    s_scanning.store(false);
}

// ══════════════════════════════════════════════════════════════════════════════
// NimBLE lifecycle
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

// Canonical IDF teardown: nimble_port_stop() makes nimble_port_run() return, at
// which point the host task frees itself via nimble_port_freertos_deinit() (see
// the task body in initNimble); nimble_port_deinit() then releases the stack and
// controller RAM (~40-60 KB). Main task only.
static void deinitNimble() {
    if (!s_nimbleInitialized.load()) return;
    stopScan();
    if (nimble_port_stop() != 0) {
        LOG_WARN("nimble_port_stop failed — keeping BLE stack resident");
        return;
    }
    nimble_port_deinit();
    s_nimbleInitialized.store(false);
    LOG_INFO("NimBLE deinitialized. Heap: %u free", esp_get_free_heap_size());
}

// ══════════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════════

void BleSensor::begin() {
    const char* addr = settings.get().bleSensorAddr;
    if (validMac(addr)) {
        taskENTER_CRITICAL(&s_mux);
        memcpy(s_targetMac, addr, 17);   // validMac guarantees exactly 17 chars
        s_targetMac[17] = '\0';
        s_addrValid = true;
        taskEXIT_CRITICAL(&s_mux);
        LOG_INFO("Target sensor: %s", addr);
    } else {
        LOG_INFO("No sensor MAC configured — scanning deferred");
    }

    s_prevFeed = (settings.get().roomSource == SL2_ROOMSRC_BLE);
    s_bleEnabled.store(settings.get().bleEnabled);

    if (!s_bleEnabled.load()) {
        LOG_INFO("BLE disabled, skipping NimBLE init");
        return;
    }

    if (!initNimble()) return;

    if (targetConfigured()) {
        startScan();
        LOG_INFO("Scanning started (auto-detect all sensor types)");
    }
}

void BleSensor::setBleEnabled(bool on) {
    s_bleEnabled.store(on);
    settings.get().bleEnabled = on;
    settings.save();

    // NimBLE bring-up/teardown is deferred to loop() — neither may run on the
    // httpd task. Disabling also hands the HP back to its internal sensor.
    s_pendingSync.store(true);
    if (!on) {
        s_pendingClear.store(true);
        s_staleReverted.store(false);
    }
    LOG_INFO("BLE %s", on ? "enabled" : "disabled");
}

bool BleSensor::isBleEnabled() {
    return s_bleEnabled.load();
}

void BleSensor::loop(CN105Controller &cn105) {
    // NimBLE follows the enable toggle (flipped from the httpd task): bring the
    // stack up or tear it down here, then let the restart drain below own the
    // scan start. Reading s_bleEnabled at drain time makes a rapid toggle
    // converge on the final state.
    if (s_pendingSync.exchange(false)) {
        if (s_bleEnabled.load()) {
            if (initNimble()) s_pendingRestart.store(true);
        } else {
            deinitNimble();
        }
    }

    // Deferred clearRemoteTemperature — kept pending until the CN105 link can
    // actually carry it, so a clear issued while disconnected isn't lost.
    // Skip it if the dial's own sensor has since become the selected AND
    // active source: LinkSensor's loop() sends promptly on selection
    // (have_sent starts false), so firing this stale clear could land after
    // that send and stomp it, leaving the pump on internal for up to
    // LinkSensor's 20s keepalive before its next resend notices and corrects.
    if (s_pendingClear.load() && cn105.isConnected()) {
        bool handedToLink = (settings.get().roomSource == SL2_ROOMSRC_LINK) &&
                             LinkSensor::isActive();
        if (!handedToLink) {
            cn105.clearRemoteTemperature();
            s_lastSentTemp = NAN;
        }
        s_pendingClear.store(false);
    }

    if (!s_bleEnabled.load()) return;

    uint32_t now = uptime_ms();
    float temp;
    uint32_t lastUpd;
    taskENTER_CRITICAL(&s_mux);
    temp    = s_temperature;
    lastUpd = s_lastUpdate;
    taskEXIT_CRITICAL(&s_mux);

    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    bool active = lastUpd > 0 && (now - lastUpd) < staleMs;
    bool stale  = lastUpd > 0 && !active;
    bool feed   = (settings.get().roomSource == SL2_ROOMSRC_BLE);

    // Radio duty: hunt hard until readings flow, then back off (see ScanProfile)
    ScanProfile want = (active && !s_discoveryMode.load()) ? ScanProfile::TRACK
                                                           : ScanProfile::SEARCH;
    if (s_scanProfile.exchange(want) != want) s_pendingRestart.store(true);

    // Deferred scan stop+restart (enable, address change, profile switch, discovery)
    if (s_pendingRestart.exchange(false)) {
        stopScan();
        if (wantScan()) startScan();
    }

    // Feed toggled from the web UI (httpd task): a falling edge hands the HP
    // back to its internal sensor, a rising edge forces a prompt send below
    if (feed != s_prevFeed) {
        s_prevFeed      = feed;
        s_lastKeepalive = 0;
        s_lastSentTemp  = NAN;
        if (!feed) s_pendingClear.store(true);
    }

    if (feed && active && cn105.isConnected() && !std::isnan(temp)) {
        // Resend on the keepalive cadence, or as soon as the value the HP will
        // actually see (quantizeRemoteTemp: 0.5 C grid + clamp) changes —
        // rate-limited so a reading jittering across a grid boundary can't
        // spam the UART
        bool changed = std::isnan(s_lastSentTemp) ||
                       CN105Controller::quantizeRemoteTemp(temp) !=
                       CN105Controller::quantizeRemoteTemp(s_lastSentTemp);
        uint32_t interval = changed ? BLE_RESEND_MIN_MS : BLE_KEEPALIVE_MS;
        if (now - s_lastKeepalive >= interval) {
            cn105.sendRemoteTemperature(temp);
            s_lastKeepalive = now;
            s_lastSentTemp  = temp;
        }
    }

    // Stale watchdog — routes the revert through the s_pendingClear mailbox
    // above, which owns the send, the retry-until-connected, and the
    // s_lastSentTemp reset
    if (feed && stale && !s_staleReverted.exchange(true)) {
        s_pendingClear.store(true);
        LOG_WARN("Sensor stale (%lus no data) — reverting to internal thermistor",
                 (unsigned long)((now - lastUpd) / 1000));
    }
}

float    BleSensor::temperature()  { return readLocked(s_temperature); }
float    BleSensor::humidity()     { return readLocked(s_humidity); }
int8_t   BleSensor::battery()      { return readLocked(s_battery); }
int      BleSensor::rssi()         { return readLocked(s_rssi); }

// Master toggle off reports neither active nor stale: the last reading survives
// in s_temperature/s_lastUpdate for a quick re-enable, but consumers (web badge,
// Dial state, HomeKit) must see the sensor as absent — not as feeding for the
// rest of the stale window, then "stale" forever.
bool BleSensor::isActive() {
    if (!s_bleEnabled.load()) return false;
    uint32_t lu = readLocked(s_lastUpdate);
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    return lu > 0 && (uptime_ms() - lu) < staleMs;
}

bool BleSensor::isStale() {
    if (!s_bleEnabled.load()) return false;
    uint32_t lu = readLocked(s_lastUpdate);
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    return lu > 0 && (uptime_ms() - lu) >= staleMs;
}

bool BleSensor::isReverted() {
    return s_staleReverted.load();
}

uint32_t BleSensor::lastUpdateAge() {
    uint32_t lu = readLocked(s_lastUpdate);
    if (lu == 0) return UINT32_MAX;
    return uptime_ms() - lu;
}

bool BleSensor::isEnabled() {
    return (settings.get().roomSource == SL2_ROOMSRC_BLE);
}

void BleSensor::setEnabled(bool enabled) {
    settings.get().roomSource = enabled ? SL2_ROOMSRC_BLE : SL2_ROOMSRC_INTERNAL;
    settings.save();
    LOG_INFO("Feed %s", enabled ? "enabled" : "disabled");
    // loop() reacts to the change: a falling edge clears the remote temp on the
    // heat pump, a rising edge sends the current reading promptly
}

void BleSensor::setAddr(const char* mac) {
    if (!mac) return;
    char* stored = settings.get().bleSensorAddr;
    constexpr size_t storedSize = sizeof(settings.get().bleSensorAddr);

    // Unchanged (also "" == "") — keep detection state, no NVS wear, no restart
    if (strcasecmp(mac, stored) == 0) return;

    bool valid = strlen(mac) > 0;
    if (valid && !validMac(mac)) {
        LOG_WARN("Invalid MAC format: %s", mac);
        return;
    }

    // Different sensor: forget the old one's identity and readings in the same
    // critical section as the target swap, so neither the feed nor the UI can
    // keep presenting the old sensor's values as current
    s_sensorType.store(nullptr);
    taskENTER_CRITICAL(&s_mux);
    memset(s_targetMac, 0, sizeof(s_targetMac));
    if (valid) memcpy(s_targetMac, mac, 17);   // validMac guarantees exactly 17 chars
    s_addrValid     = valid;
    bool hadReading = s_lastUpdate != 0;
    s_temperature   = NAN;
    s_humidity      = NAN;
    s_battery       = -1;
    s_rssi          = 0;
    s_lastUpdate    = 0;
    taskEXIT_CRITICAL(&s_mux);
    s_staleReverted.store(false);

    // If the old reading may be live in the HP, hand it back to its internal
    // thermistor now rather than after the stale timeout
    if (hadReading && (settings.get().roomSource == SL2_ROOMSRC_BLE)) s_pendingClear.store(true);

    strncpy(stored, mac, storedSize - 1);
    stored[storedSize - 1] = '\0';
    settings.save();
    s_pendingRestart.store(true);
    if (valid) {
        LOG_INFO("Sensor address set: %s", mac);
    } else {
        LOG_INFO("Sensor address cleared");
    }
}

const char* BleSensor::getAddr() {
    return settings.get().bleSensorAddr;
}

const char* BleSensor::sensorType() {
    return s_sensorType.load();
}

void BleSensor::startDiscovery() {
    if (!s_bleEnabled.load()) {
        LOG_WARN("BLE discovery rejected — BLE not enabled");
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_discoveryCount     = 0;
    s_discoveryTruncated = false;
    s_lastPushedCount    = 0;
    taskEXIT_CRITICAL(&s_mux);
    s_discoveryStart.store(uptime_ms());
    s_discoveryMode.store(true);

    // The enable toggle may still have its NimBLE init queued (loop() drains it
    // at 1 Hz) — queue again so a Scan tap right after enabling still works
    if (!s_nimbleInitialized.load()) s_pendingSync.store(true);
    s_pendingRestart.store(true);   // (re)start with the SEARCH profile

    LOG_INFO("Discovery scan started (%lums)", (unsigned long)BLE_DISCOVERY_MS);
}

bool BleSensor::isDiscovering() {
    return s_discoveryMode.load();
}

bool BleSensor::pollDiscoveryUpdate() {
    if (!s_discoveryMode.load()) return false;
    taskENTER_CRITICAL(&s_mux);
    bool fresh = s_discoveryCount > s_lastPushedCount;
    if (fresh) s_lastPushedCount = s_discoveryCount;
    taskEXIT_CRITICAL(&s_mux);
    return fresh;
}

bool BleSensor::pollDiscoveryComplete() {
    if (s_discoveryMode.load() &&
        uptime_ms() - s_discoveryStart.load() >= BLE_DISCOVERY_MS) {
        s_discoveryMode.store(false);

        // Stop scanning if no target configured; otherwise the next loop() pass
        // restores the TRACK profile via the usual restart path
        if (!targetConfigured() && s_scanning.load()) {
            stopScan();
        }

        LOG_INFO("Discovery complete: %d sensor(s) found", readLocked(s_discoveryCount));
        return true;
    }
    return false;
}

int BleSensor::discoveryResults(BleDiscoveredDevice* out, int max, bool* truncated) {
    taskENTER_CRITICAL(&s_mux);
    int n = s_discoveryCount < max ? s_discoveryCount : max;
    for (int i = 0; i < n; i++) out[i] = s_discovered[i];
    if (truncated) *truncated = s_discoveryTruncated;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

#endif // BLE_ENABLE
