#include "ble_config.h"

#ifdef BLE_ENABLE

#include "ble_pair.h"
#include "ble_pair_policy.h"
#include "ble_sensor.h"
#include "settings.h"
#include "room_avg.h"
#include "status_led.h"
#include "web_server.h"     // webota_active()
#include "espnow_link.h"
#include "logging.h"
#include "esp_utils.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <freertos/FreeRTOS.h>

static const char* TAG = "blepair";

static_assert(BLEPAIR_MAX_SLOTS == ROOM_MAX_BLE_SENSORS,
              "pairing policy slot count must match the settings sensor list");

// ── State ───────────────────────────────────────────────────────────────────
// The policy and the name cache are written from the NimBLE host task and read
// from the main task, so both live under s_pmux — the same discipline
// addDiscoveryResult uses in ble_sensor.cpp.
static portMUX_TYPE s_pmux = portMUX_INITIALIZER_UNLOCKED;
static PairPolicy   s_policy;

// Display names are presentation, not policy, so they are cached here rather
// than pushed into the pure module.
struct NameEntry { char mac[18]; char name[24]; const char* type; };
static NameEntry s_names[BLEPAIR_MAX_CANDIDATES];
static int       s_nameCount = 0;

static std::atomic<bool> s_listening{false};
static uint32_t s_started   = 0;                // main task only
static bool     s_forcedBle = false;            // window turned BLE on transiently
static bool     s_baselined = false;            // freezeBaselines() already run this window
static PairSlotView s_view  = {};               // baselines snapshotted at open

// ── Helpers ─────────────────────────────────────────────────────────────────
//
// Outcomes go to LOG_* only, not the event log: eventlog_append() writes NVS
// and event_log.h is explicit that it is for device-level faults, not
// user-driven actions that can repeat freely. The log ring already surfaces
// these lines in the web UI.

static void fail(const char* why) {
    LOG_WARN("[BlePair] %s", why);
#if PIN_LED_DATA >= 0
    statusLED.requestHold(SLED_RESULT_FAIL, 3000);
#endif
}

// Close without committing: the transient BLE enable is rolled back so a
// failed pair leaves the user's persistent toggle untouched — UNLESS the web
// UI enabled BLE for real while the window was open, in which case
// settings.bleEnabled now reads true and there is nothing to roll back (doing
// so anyway would tear NimBLE down under a setting the user just turned on).
static void closeWindow() {
    s_listening.store(false);
    BleSensor::setPairMode(false);
    if (s_forcedBle) {
        s_forcedBle = false;
        if (!settings.get().bleEnabled) BleSensor::setBleEnabledTransient(false);
    }
}

// "AA:BB:CC:DD:EE:FF" -> uppercase in place, matching the web UI's convention.
static void upperMac(char* mac) {
    for (int i = 0; i < 17 && mac[i]; i++)
        if (mac[i] >= 'a' && mac[i] <= 'f') mac[i] = (char)(mac[i] - 32);
}

static char upperHex(char c) { return (c >= 'a' && c <= 'f') ? (char)(c - 32) : c; }

static void lookupName(const char* mac, char* out, size_t outLen) {
    char advName[24] = "";
    const char* type = nullptr;

    taskENTER_CRITICAL(&s_pmux);
    for (int i = 0; i < s_nameCount; i++) {
        if (strcasecmp(s_names[i].mac, mac) == 0) {
            memcpy(advName, s_names[i].name, sizeof(advName));
            type = s_names[i].type;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_pmux);

    if (advName[0]) { snprintf(out, outLen, "%s", advName); return; }
    // No advertised name: "<type> EE:FF" from the last two MAC octets, in the
    // same uppercase convention as a stored address.
    snprintf(out, outLen, "%s %c%c:%c%c", type ? type : "Sensor",
             upperHex(mac[12]), upperHex(mac[13]), upperHex(mac[15]), upperHex(mac[16]));
}

// Snapshot what the policy needs to know about the slots, including each
// configured sensor's last recorded signal strength. That is only the FALLBACK
// baseline for the rise test — it is a single frame that may be as old as
// roomStaleTimeoutS. The baseline that normally applies is frozen from the
// window's own first BLEPAIR_BASELINE_MS (PairPolicy::freezeBaselines), which
// is why the documented flow is press first, then approach.
static void snapshotSlots() {
    s_view = {};
    s_view.feedSlot = BleSensor::feedSlot();
    s_view.uptimeMs = uptime_ms();
    for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++) {
        const BleSensorCfg& cfg = settings.get().bleSensors[i];
        s_view.slot[i].configured = cfg.addr[0] != '\0';
        snprintf(s_view.slot[i].mac, sizeof(s_view.slot[i].mac), "%s", cfg.addr);
        s_view.slot[i].seenSinceBoot = BleSensor::slotSeenSinceBoot(i);
        bool valid = false;
        int  rssi  = BleSensor::slotRssi(i, &valid);
        s_view.slot[i].baselineRssi  = (int8_t)rssi;
        s_view.slot[i].baselineValid = valid;
    }
}

static void commit(const PairVerdict& v) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", v.mac);
    upperMac(mac);

    // Re-validate against LIVE settings, not the window-open snapshot: the
    // httpd task can fill, clear, or relocate slots while the window runs. If
    // this MAC has meanwhile been added to a DIFFERENT slot (e.g. from the
    // phone during the window), retarget the write there instead of creating
    // a duplicate entry — setSensor() does not dedupe; web_ws.cpp's bleAddMac
    // command is the one place that already scans-then-writes this way.
    int slot = -1;
    for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++) {
        if (strcasecmp(settings.get().bleSensors[i].addr, mac) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        // A non-empty target is normally someone filling a free slot from the
        // phone mid-window — refuse. But a RECLAIM verdict deliberately picked
        // an occupied-but-dead slot, so for that one the emptiness test is not
        // the right question; re-run the reclaim conditions against live state
        // instead. Without this the reclaim path could never commit at all.
        if (settings.get().bleSensors[v.slot].addr[0]) {
            // The MAC test is NOT redundant with seenSinceBoot: setSensor()
            // resets the slot's readings on a MAC change, so a sensor the phone
            // added seconds ago reads seenSinceBoot == false exactly like a dead
            // one, and reclaiming it would silently delete it. Comparing against
            // the window-open snapshot is what tells the two apart — a real
            // reclaim leaves the address untouched (a UI rename writes cfg.name,
            // never cfg.addr), and the reclaim path only runs when every slot
            // was configured, so the snapshot MAC is never empty here.
            const bool stillReclaimable = v.reclaim &&
                                          !BleSensor::slotSeenSinceBoot(v.slot) &&
                                          v.slot != BleSensor::feedSlot() &&
                                          strcasecmp(settings.get().bleSensors[v.slot].addr,
                                                     s_view.slot[v.slot].mac) == 0;
            if (!stillReclaimable) {
                fail("slot taken during pairing");
                return;
            }
        }
        slot = v.slot;
    }

    // Keep the stored display name when this slot already holds this MAC:
    // re-promotion is a supported gesture and the user may have renamed the
    // sensor ("Bedroom") in the web UI. setSensor() treats nullptr as
    // keep-stored-name. A new or reclaimed slot gets a generated one.
    char        name[24]  = "";
    const char* namePtr   = nullptr;
    if (strcasecmp(settings.get().bleSensors[slot].addr, mac) != 0) {
        lookupName(v.mac, name, sizeof(name));
        namePtr = name;
    }

    BleSensor::setSensor(slot, mac, namePtr);          // persists; resets the slot's readings

    // Make the window's transient enable permanent. isBleEnabled() already
    // reads true when the window forced it on, so test the PERSISTED value.
    if (!settings.get().bleEnabled) BleSensor::setBleEnabled(true);
    s_forcedBle = false;                              // survives closeWindow()

    // The freshly paired sensor always becomes the room source. setSensor must
    // run first: a BLE member bit is only available once its slot is configured.
    const int bit = ROOM_MEMBER_BLE0 + slot;
    bool fed = RoomAvg::memberAvailable(bit);
    if (fed) {
        settings.get().roomMode   = 0;                // single
        settings.get().roomSingle = (uint8_t)bit;
        settings.save();                              // roomSource derives on save
    } else {
        LOG_WARN("[BlePair] slot %d not selectable, sensor stored but not fed", slot);
    }

    LOG_INFO("[BlePair] paired %s (%s) into slot %d at %d dBm",
             mac, settings.get().bleSensors[slot].name, slot, (int)v.medianRssi);
    // The LED is the only feedback this feature has, so green must mean the
    // pump is actually reading the sensor — "stored but not fed" is a failure
    // from where the user is standing.
#if PIN_LED_DATA >= 0
    statusLED.requestHold(fed ? SLED_RESULT_OK : SLED_RESULT_FAIL, 3000);
#endif
}

// ── Public API ──────────────────────────────────────────────────────────────

void BlePair::begin() {
    taskENTER_CRITICAL(&s_pmux);
    s_policy.reset();
    s_nameCount = 0;
    taskEXIT_CRITICAL(&s_pmux);
}

bool BlePair::isListening() { return s_listening.load(); }

void BlePair::observeAdvert(const char* mac, int rssi, bool tempDecoded,
                            const char* advName, const char* type) {
    if (!s_listening.load()) return;

    // Prep strings before taking the lock — this runs on the NimBLE host task
    // with interrupts off inside the critical section, and addDiscoveryResult
    // (ble_sensor.cpp) already establishes the convention: only the search +
    // write happen under the lock, string formatting stays outside it.
    char macBuf[18]  = {};
    char nameBuf[24] = {};
    snprintf(macBuf,  sizeof(macBuf),  "%s", mac);
    snprintf(nameBuf, sizeof(nameBuf), "%s", advName ? advName : "");

    taskENTER_CRITICAL(&s_pmux);
    s_policy.observe(mac, rssi, tempDecoded);
    if (tempDecoded) {
        int found = -1;
        for (int i = 0; i < s_nameCount; i++)
            if (strcasecmp(s_names[i].mac, macBuf) == 0) { found = i; break; }
        if (found < 0 && s_nameCount < BLEPAIR_MAX_CANDIDATES) found = s_nameCount++;
        if (found >= 0) {
            memcpy(s_names[found].mac,  macBuf,  sizeof(s_names[found].mac));
            memcpy(s_names[found].name, nameBuf, sizeof(s_names[found].name));
            s_names[found].type = type;
        }
    }
    taskEXIT_CRITICAL(&s_pmux);
}

void BlePair::onTripleClick() {
    if (s_listening.load()) {
        LOG_INFO("[BlePair] window cancelled by button");
        closeWindow();
        return;
    }
    if (webota_active()) { fail("pairing refused: OTA in progress"); return; }
#if ESPNOW_REMOTE_ENABLE
    if (espnowLink.pairingActive()) { fail("pairing refused: Link pairing open"); return; }
#endif

    snapshotSlots();
    taskENTER_CRITICAL(&s_pmux);
    s_policy.reset();
    s_nameCount = 0;
    taskEXIT_CRITICAL(&s_pmux);

    s_started   = uptime_ms();
    s_baselined = false;
    s_listening.store(true);
    BleSensor::setPairMode(true);      // forces the SEARCH duty profile
    // A unit with BLE switched off would otherwise open a window that never
    // scans — the scan gate also tests s_bleEnabled.
    s_forcedBle = !BleSensor::isBleEnabled();
    if (s_forcedBle) BleSensor::setBleEnabledTransient(true);
    LOG_INFO("[BlePair] window open — bring the sensor to the unit");
}

void BlePair::loop() {
    if (!s_listening.load()) return;

    if (webota_active()) {
        closeWindow();
        fail("pairing aborted: OTA started");
        return;
    }

    const uint32_t elapsed = (uint32_t)(uptime_ms() - s_started);
    s_view.uptimeMs = uptime_ms();

    // Freeze the rise-test baselines once, from the window's own opening
    // seconds. BLEPAIR_BASELINE_MS < BLEPAIR_EARLY_MS (static_assert in the
    // policy header) guarantees this runs before any accept can fire.
    if (!s_baselined && elapsed >= BLEPAIR_BASELINE_MS) {
        s_baselined = true;
        taskENTER_CRITICAL(&s_pmux);
        s_policy.freezeBaselines();
        taskEXIT_CRITICAL(&s_pmux);
    }

    PairVerdict v;
    taskENTER_CRITICAL(&s_pmux);
    v = s_policy.evaluate(s_view, elapsed);
    taskEXIT_CRITICAL(&s_pmux);

    if (v.result == PR_PENDING) return;

    // Commit BEFORE closing: on success it makes the transient BLE enable
    // permanent and clears s_forcedBle, so closeWindow() does not roll back an
    // enable the stored sensor now depends on.
    if (v.result == PR_ACCEPT) commit(v);

    closeWindow();
    switch (v.result) {
        case PR_ACCEPT:       break;   // handled above
        case PR_AMBIGUOUS:    fail("two sensors too close to tell apart"); break;
        case PR_NO_CANDIDATE: fail("no sensor found near the unit"); break;
        case PR_SLOTS_FULL:   fail("all sensor slots are in use"); break;
        default: break;
    }
}

#endif // BLE_ENABLE
