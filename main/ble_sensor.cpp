#include "ble_config.h"

#ifdef BLE_ENABLE

#include "ble_sensor.h"
#include "ble_pair.h"
#include "settings.h"
#include "sl2_proto.h"
#include "logging.h"
#include "esp_utils.h"
#include "link_sensor.h"
#include "room_avg.h"

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

// ── Per-slot state: scan target + latest reading (guarded by s_mux — the
//    setters rewrite targets from the httpd task while the scan callback
//    compares against them) ────────────────────────────────────────────────
struct SlotState {
    char        mac[18];      // "aa:bb:cc:dd:ee:ff", lower-cased by setSlotMac()
    bool        valid;        // mac holds a parseable target
    float       temp;
    float       hum;
    int8_t      batt;
    int         rssi;
    uint32_t    lastUpdate;
    const char* type;         // detected decoder type (string literal, nullptr = unknown)
};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SlotState s_slots[ROOM_MAX_BLE_SENSORS];   // readings reset in begin()/setSensor()

// Reset a slot's readings/identity, keeping its target. Call under s_mux.
static void resetSlotReadings(SlotState &sl) {
    sl.temp = NAN; sl.hum = NAN; sl.batt = -1; sl.rssi = 0;
    sl.lastUpdate = 0; sl.type = nullptr;
}

// Store a target in the scan callback's own lower-case form so the per
// advertisement match can be a fixed-length memcmp instead of strcasecmp —
// that compare runs under s_mux, i.e. with interrupts disabled. validMac()
// guarantees exactly 17 characters. Call under s_mux.
static void setSlotMac(SlotState &sl, const char* mac) {
    for (int i = 0; i < 17; i++) {
        char c = mac[i];
        sl.mac[i] = (c >= 'A' && c <= 'F') ? (char)(c + 32) : c;
    }
    sl.mac[17] = '\0';
}

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
static std::atomic<bool> s_pairMode{false};        // BlePair window open (ble_pair.cpp)

// ── Keepalive state (main task only) ────────────────────────────────────────
static uint32_t s_lastKeepalive = 0;
static float    s_lastSentTemp  = NAN;   // last value sent to the HP
static int      s_prevFeedSlot  = -1;    // feed edge detection (-1 = not feeding)

// ── Discovery state ─────────────────────────────────────────────────────────
// Results are written from the NimBLE host task, cleared from the httpd task
// (startDiscovery) and read from the main task — array, count, truncated flag,
// pushed-count and generation all live under s_mux. The generation is what
// lets a reader that dropped the lock mid-pass notice that the list it sampled
// was cleared out from under it.
static BleDiscoveredDevice s_discovered[BLE_MAX_DISCOVERED];
static int      s_discoveryCount     = 0;
static bool     s_discoveryTruncated = false;
static int      s_lastPushedCount    = 0;
static uint32_t s_discoveryGen       = 0;   // bumped every time the list is cleared
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
    taskENTER_CRITICAL(&s_mux);
    bool any = false;
    for (const SlotState &sl : s_slots) any = any || sl.valid;
    taskEXIT_CRITICAL(&s_mux);
    return any;
}

// Which slot the legacy single-source feed reads from: single mode with a BLE
// member picked. -1 when averaging or a non-BLE source is selected. Public —
// link_sensor/espnow_link/room_avg use it for their arbitration checks.
int BleSensor::feedSlot() {
    const DeviceSettings &st = settings.get();
    if (st.roomMode != 0 || st.roomSingle < ROOM_MEMBER_BLE0) return -1;
    int idx = st.roomSingle - ROOM_MEMBER_BLE0;
    return idx < ROOM_MAX_BLE_SENSORS ? idx : -1;
}

// The sensor that stands for "the remote sensor" to consumers that can only
// show one (see ble_sensor.h). Prefer the one actually driving the pump; in
// Average mode, or with a non-BLE source selected, fall back to the first
// configured slot so the HomeKit accessory keeps existing instead of
// appearing and vanishing with the room-source setting.
int BleSensor::primarySlot() {
    int fed = feedSlot();
    if (fed >= 0 && settings.get().bleSensors[fed].addr[0]) return fed;
    for (int i = 0; i < ROOM_MAX_BLE_SENSORS; i++)
        if (settings.get().bleSensors[i].addr[0]) return i;
    return -1;
}

// One home for the freshness rule shared by loop()/isActive()/isStale().
static bool freshAt(uint32_t lastUpdate, uint32_t now, uint32_t staleMs) {
    return lastUpdate > 0 && (now - lastUpdate) < staleMs;
}

// A scan should be running whenever there's a target to track or a discovery
// window open (callers also check NimBLE is up and BLE enabled)
static bool wantScan() {
    return targetConfigured() || s_discoveryMode.load() || s_pairMode.load();
}

// ── MAC format validator: "AA:BB:CC:DD:EE:FF" (any case) ───────────────────
static bool validMac(const char* str) {
    if (!str || strlen(str) != 17) return false;
    unsigned int b[6];
    return sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
                  &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6;
}

// Record a discovered device (deduplicated by MAC). Runs in the NimBLE scan
// callback — the one and only writer of the results array — so the dedup
// search can read entries below the count unlocked. The count that bounds it
// is not ours alone (startDiscovery() zeroes it from the httpd task), so it is
// sampled under s_mux together with a generation the append re-checks. String
// prep stays outside the lock too: s_mux is a spinlock, and up to
// BLE_MAX_DISCOVERED compares with interrupts off is latency the BLE/WiFi
// coexistence can't spare.
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

    const bool hasTemp = !std::isnan(temp);
    const bool hasHum  = !std::isnan(hum);

    // Entries below the count are only ever appended and refreshed by this
    // callback, so scanning them unlocked can't race a writer — the count
    // itself can, hence the stamped snapshot. Both sides are upper-cased
    // 17-char MACs, so the compare is a memcmp.
    taskENTER_CRITICAL(&s_mux);
    uint32_t gen  = s_discoveryGen;
    int      seen = s_discoveryCount;
    taskEXIT_CRITICAL(&s_mux);

    int hit = -1;
    for (int j = 0; j < seen; j++) {
        if (memcmp(s_discovered[j].addr, d.addr, 17) == 0) { hit = j; break; }
    }
    if (hit < 0 && !hasTemp) return;   // nothing to refresh, nothing to list

    taskENTER_CRITICAL(&s_mux);
    // A newer generation means startDiscovery() cleared the list while we
    // scanned: the index points at a dead entry, and the list that replaced it
    // is still empty (this callback is its only writer, and it is single
    // threaded), so fall through to the append.
    if (s_discoveryGen != gen) hit = -1;
    if (hit >= 0) {
        // Already seen — refresh RSSI and the latest valid reading
        s_discovered[hit].rssi = rssi;
        if (hasTemp) s_discovered[hit].temperature = temp;
        if (hasHum)  s_discovered[hit].humidity    = hum;
    } else if (hasTemp) {
        // List only once a temperature decodes — a battery-only frame can't identify it
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

        // Discovery and proximity pairing both want any sensor-type device,
        // not just configured targets. Decode once and fan out.
        if (s_discoveryMode.load() || s_pairMode.load()) {
            SensorReading r;
            const char* type = decodeAdvertisement(disc->data, disc->length_data, addrStr, r);
            if (type) {
                char name[24];
                extractDeviceName(disc->data, disc->length_data, name, sizeof(name));
                if (s_discoveryMode.load())
                    addDiscoveryResult(addrStr, name, type, disc->rssi, r.temp, r.hum);
                if (s_pairMode.load())
                    BlePair::observeAdvert(addrStr, disc->rssi, !std::isnan(r.temp), name, type);
            }
        }

        // MAC filter: only process configured target sensors. Match under the
        // lock — setSensor rewrites targets from the httpd task. Both sides are
        // lower-case 17-char MACs (setSlotMac), so this is a memcmp: the loop
        // runs once per advertisement with interrupts off, where strcasecmp's
        // per-character case folding is latency nobody asked for.
        taskENTER_CRITICAL(&s_mux);
        int slot = -1;
        for (int i = 0; i < ROOM_MAX_BLE_SENSORS; i++) {
            if (s_slots[i].valid && memcmp(addrStr, s_slots[i].mac, 17) == 0) { slot = i; break; }
        }
        taskEXIT_CRITICAL(&s_mux);
        if (slot < 0) return 0;

        // Decode the live advertisement and publish the freshest reading
        SensorReading reading;
        const char* liveType = decodeAdvertisement(disc->data, disc->length_data, addrStr, reading);
        if (liveType) {
            bool gotTemp = !std::isnan(reading.temp);
            taskENTER_CRITICAL(&s_mux);
            SlotState &sl = s_slots[slot];
            if (gotTemp) {
                sl.temp = reading.temp;
                // Freshness follows the temperature: battery-only frames from
                // split-field sensors (SwitchBot Pro family) must not keep the
                // stale-revert watchdog from firing
                sl.lastUpdate = uptime_ms();
            }
            if (!std::isnan(reading.hum))  sl.hum  = reading.hum;
            if (reading.batt >= 0)         sl.batt = reading.batt;
            sl.rssi = disc->rssi;
            bool firstType = (sl.type == nullptr);
            sl.type = liveType;
            taskEXIT_CRITICAL(&s_mux);

            // Only a reading from the slot that actually feeds the pump may
            // rearm the stale watchdog — another sensor's chatter must not.
            if (gotTemp && slot == BleSensor::feedSlot()) s_staleReverted.store(false);
            if (firstType) {
                LOG_INFO("Detected sensor type: %s (slot %d)", liveType, slot);
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
    int nTargets = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < ROOM_MAX_BLE_SENSORS; i++) {
        SlotState &sl = s_slots[i];
        resetSlotReadings(sl);
        const char* addr = settings.get().bleSensors[i].addr;
        sl.valid = validMac(addr);
        memset(sl.mac, 0, sizeof(sl.mac));
        if (sl.valid) { setSlotMac(sl, addr); nTargets++; }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (nTargets) {
        LOG_INFO("%d target sensor(s) configured", nTargets);
    } else {
        LOG_INFO("No sensor MAC configured — scanning deferred");
    }

    s_prevFeedSlot = feedSlot();
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

void BleSensor::setPairMode(bool on) {
    if (s_pairMode.exchange(on) == on) return;
    // Duty profile changes with the mode; let loop() apply it rather than
    // touching NimBLE from the caller's task.
    s_pendingRestart.store(true);
    LOG_INFO("BLE pair mode %s", on ? "opened" : "closed");
}

void BleSensor::setBleEnabledTransient(bool on) {
    if (s_bleEnabled.exchange(on) == on) return;
    // NimBLE bring-up/teardown is deferred to loop(), same as setBleEnabled().
    s_pendingSync.store(true);
    // Turning BLE off does not clear roomSingle, so a BLE slot can still be the
    // selected room source here — and once s_bleEnabled is false, loop()
    // returns early past both the feed and the stale watchdog. Without this the
    // pump would be left chasing a temperature that can never update again, and
    // would stay that way across reboots. Hand it back to its internal
    // thermistor, exactly as setBleEnabled(false) does.
    if (!on && feedSlot() >= 0) {
        s_pendingClear.store(true);
        s_staleReverted.store(false);
    }
    LOG_INFO("BLE %s (transient, not persisted)", on ? "enabled" : "disabled");
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
        bool handedToLink = (settings.get().roomMode == 0) &&
                            (settings.get().roomSingle == ROOM_MEMBER_LINK) &&
                             LinkSensor::isActive();
        // Same reasoning for a switch into Average mode: the blend owns the
        // register now, and this stale clear would stomp its value.
        bool handedToAvg = RoomAvg::isFeeding();
        if (!handedToLink && !handedToAvg) {
            cn105.clearRemoteTemperature();
            s_lastSentTemp = NAN;
        }
        s_pendingClear.store(false);
    }

    if (!s_bleEnabled.load()) return;

    uint32_t now = uptime_ms();
    int  slot = feedSlot();
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;

    float temp = NAN;
    uint32_t lastUpd = 0;
    bool allConfiguredActive = true;
    taskENTER_CRITICAL(&s_mux);
    if (slot >= 0) {
        temp    = s_slots[slot].temp;
        lastUpd = s_slots[slot].lastUpdate;
    }
    for (const SlotState &sl : s_slots) {
        if (sl.valid && !freshAt(sl.lastUpdate, now, staleMs))
            allConfiguredActive = false;
    }
    taskEXIT_CRITICAL(&s_mux);

    bool active = lastUpd > 0 && (now - lastUpd) < staleMs;
    bool stale  = lastUpd > 0 && !active;
    bool feed   = (slot >= 0);

    // Radio duty: hunt hard until every configured sensor's readings flow,
    // then back off (see ScanProfile)
    ScanProfile want = (allConfiguredActive && !s_discoveryMode.load() && !s_pairMode.load())
                       ? ScanProfile::TRACK
                       : ScanProfile::SEARCH;
    if (s_scanProfile.exchange(want) != want) s_pendingRestart.store(true);

    // Deferred scan stop+restart (enable, address change, profile switch, discovery)
    if (s_pendingRestart.exchange(false)) {
        stopScan();
        if (wantScan()) startScan();
    }

    // Feed toggled from the web UI (httpd task), or moved to a different
    // sensor slot: a falling edge hands the HP back to its internal sensor, a
    // rising edge (or slot move) forces a prompt send below
    if (slot != s_prevFeedSlot) {
        s_prevFeedSlot  = slot;
        s_lastKeepalive = 0;
        s_lastSentTemp  = NAN;
        if (!feed) s_pendingClear.store(true);
    }

    if (feed && active && cn105.isConnected() && !std::isnan(temp)) {
        // Calibration offset applies here, not just in the Average blend — a
        // user who corrects a sensor that reads high expects the heat pump to
        // see the corrected value in either mode. Applied before the change
        // test so the 0.5 C grid quantizes what the HP will actually see.
        float feedTemp = room_apply_offset(settings.get(),
                                           ROOM_MEMBER_BLE0 + slot, temp);
        // Resend on the keepalive cadence, or as soon as the value the HP will
        // actually see (quantizeRemoteTemp: 0.5 C grid + clamp) changes —
        // rate-limited so a reading jittering across a grid boundary can't
        // spam the UART
        bool changed = std::isnan(s_lastSentTemp) ||
                       CN105Controller::quantizeRemoteTemp(feedTemp) !=
                       CN105Controller::quantizeRemoteTemp(s_lastSentTemp);
        uint32_t interval = changed ? BLE_RESEND_MIN_MS : BLE_KEEPALIVE_MS;
        if (now - s_lastKeepalive >= interval) {
            cn105.sendRemoteTemperature(feedTemp);
            s_lastKeepalive = now;
            s_lastSentTemp  = feedTemp;
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

static bool slotIdxOk(int idx) { return idx >= 0 && idx < ROOM_MAX_BLE_SENSORS; }

float    BleSensor::temperature(int idx) { return slotIdxOk(idx) ? readLocked(s_slots[idx].temp) : NAN; }
float    BleSensor::humidity(int idx)    { return slotIdxOk(idx) ? readLocked(s_slots[idx].hum)  : NAN; }
int8_t   BleSensor::battery(int idx)     { return slotIdxOk(idx) ? readLocked(s_slots[idx].batt) : -1; }
int      BleSensor::rssi(int idx)        { return slotIdxOk(idx) ? readLocked(s_slots[idx].rssi) : 0; }
const char* BleSensor::sensorType(int idx) { return slotIdxOk(idx) ? readLocked(s_slots[idx].type) : nullptr; }
bool     BleSensor::isConfigured(int idx) { return slotIdxOk(idx) && readLocked(s_slots[idx].valid); }

bool BleSensor::slotSeenSinceBoot(int idx) {
    if (!slotIdxOk(idx)) return false;
    taskENTER_CRITICAL(&s_mux);
    bool seen = s_slots[idx].lastUpdate != 0;
    taskEXIT_CRITICAL(&s_mux);
    return seen;
}

int BleSensor::slotRssi(int idx, bool* valid) {
    if (valid) *valid = false;
    if (!slotIdxOk(idx)) return 0;
    // Validity means FRESH, not "ever seen": a slot silent past the stale
    // timeout must not supply a rise-test baseline (a sensor that dies, gets
    // a battery swap, and is held to the unit again must be re-promotable —
    // see PairPolicy's rise test, which applies to every configured slot).
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    taskENTER_CRITICAL(&s_mux);
    uint32_t lu   = s_slots[idx].lastUpdate;
    int      rssi = s_slots[idx].rssi;
    taskEXIT_CRITICAL(&s_mux);
    bool has = freshAt(lu, uptime_ms(), staleMs);
    if (valid) *valid = has;
    return has ? rssi : 0;
}

// Master toggle off reports neither active nor stale: the last readings
// survive in the slots for a quick re-enable, but consumers (web badge, Dial
// state, HomeKit) must see the sensors as absent — not as feeding for the
// rest of the stale window, then "stale" forever.
bool BleSensor::isActive(int idx) {
    if (!s_bleEnabled.load() || !slotIdxOk(idx)) return false;
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    return freshAt(readLocked(s_slots[idx].lastUpdate), uptime_ms(), staleMs);
}

bool BleSensor::isStale(int idx) {
    if (!s_bleEnabled.load() || !slotIdxOk(idx)) return false;
    uint32_t lu = readLocked(s_slots[idx].lastUpdate);
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    return lu > 0 && !freshAt(lu, uptime_ms(), staleMs);
}

bool BleSensor::isReverted() {
    return s_staleReverted.load();
}

uint32_t BleSensor::lastUpdateAge(int idx) {
    if (!slotIdxOk(idx)) return UINT32_MAX;
    uint32_t lu = readLocked(s_slots[idx].lastUpdate);
    if (lu == 0) return UINT32_MAX;
    return uptime_ms() - lu;
}

bool BleSensor::isEnabled() {
    return feedSlot() >= 0;
}

void BleSensor::setSensor(int idx, const char* mac, const char* name) {
    if (!mac || !slotIdxOk(idx)) return;
    BleSensorCfg &cfg = settings.get().bleSensors[idx];

    bool valid = strlen(mac) > 0;
    if (valid && !validMac(mac)) {
        LOG_WARN("Invalid MAC format: %s", mac);
        return;
    }

    bool macChanged = strcasecmp(mac, cfg.addr) != 0;
    bool nameChanged = name && strncmp(name, cfg.name, sizeof(cfg.name) - 1) != 0;
    if (!macChanged && !nameChanged) return;  // no NVS wear, no restart

    if (macChanged) {
        // Different sensor: forget the old one's identity and readings in the
        // same critical section as the target swap, so neither the feed nor
        // the UI can keep presenting the old sensor's values as current
        taskENTER_CRITICAL(&s_mux);
        SlotState &sl = s_slots[idx];
        memset(sl.mac, 0, sizeof(sl.mac));
        if (valid) setSlotMac(sl, mac);
        sl.valid = valid;
        bool hadReading = sl.lastUpdate != 0;
        resetSlotReadings(sl);
        taskEXIT_CRITICAL(&s_mux);
        if (idx == feedSlot()) s_staleReverted.store(false);

        // If the old reading may be live in the HP, hand it back to its
        // internal thermistor now rather than after the stale timeout
        if (hadReading && idx == feedSlot()) s_pendingClear.store(true);

        strncpy(cfg.addr, mac, sizeof(cfg.addr) - 1);
        cfg.addr[sizeof(cfg.addr) - 1] = '\0';
        // A cleared slot leaves the average and drops its calibration; a
        // replaced sensor must not inherit the old one's offset either.
        settings.get().roomMembers &= (uint8_t)~(1u << (ROOM_MEMBER_BLE0 + idx));
        settings.get().roomOffsets[ROOM_MEMBER_BLE0 + idx] = 0;
        if (!valid) {
            cfg.name[0] = '\0';
            // A cleared slot can't stay the single-mode pick: without this,
            // roomSingle points at an empty slot and the pump quietly runs on
            // internal while the UI shows a source that no longer exists. A
            // REPLACED sensor keeps the selection on purpose (sensor swap).
            if (settings.get().roomSingle == ROOM_MEMBER_BLE0 + idx)
                settings.get().roomSingle = ROOM_MEMBER_INTERNAL;
        }
    }
    if (name) {
        strncpy(cfg.name, name, sizeof(cfg.name) - 1);
        cfg.name[sizeof(cfg.name) - 1] = '\0';
    }
    settings.save();
    if (macChanged) s_pendingRestart.store(true);
    LOG_INFO("Sensor slot %d %s: %s", idx, valid ? "set" : "cleared", valid ? mac : "");
}

void BleSensor::renameSensor(int idx, const char* name) {
    if (!name || !slotIdxOk(idx)) return;
    const BleSensorCfg &cfg = settings.get().bleSensors[idx];
    if (!cfg.addr[0]) return;                 // empty slot has nothing to name
    setSensor(idx, cfg.addr, name);           // same MAC -> only the name path runs
}

const char* BleSensor::getAddr() {
    int idx = primarySlot();
    static const char kNone[] = "";
    return idx >= 0 ? settings.get().bleSensors[idx].addr : kNone;
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
    s_discoveryGen++;    // voids indices readers sampled from the old list
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
    // Take the count and generation first, then copy a few devices per
    // critical section: a full list is ~1.3 KB, and every byte of it under
    // this spinlock is a byte of interrupt latency on the caller's core.
    // Within one scan that costs nothing but freshness — the list only grows
    // and its entries are only refreshed in place, so every device still
    // crosses the lock whole and at worst the head carries readings one scan
    // tick older than the tail (batches are copied head-first). What it must not do is mix two scans, so each
    // batch re-checks the generation and abandons the copy if a new scan
    // cleared the list underneath it.
    constexpr int COPY_BATCH = 4;
    taskENTER_CRITICAL(&s_mux);
    uint32_t gen = s_discoveryGen;
    int n = s_discoveryCount < max ? s_discoveryCount : max;
    if (truncated) *truncated = s_discoveryTruncated;
    taskEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < n; i += COPY_BATCH) {
        int end = i + COPY_BATCH < n ? i + COPY_BATCH : n;
        taskENTER_CRITICAL(&s_mux);
        bool cleared = s_discoveryGen != gen;
        if (!cleared) {
            for (int j = i; j < end; j++) out[j] = s_discovered[j];
        }
        taskEXIT_CRITICAL(&s_mux);
        // Everything copied so far belongs to the scan that just ended, so
        // report none of it; the new scan's first devices arrive in the next
        // push, a second or so away.
        if (cleared) {
            if (truncated) *truncated = false;
            return 0;
        }
    }
    return n;
}

#endif // BLE_ENABLE
