#include "link_sensor.h"
#include "room_feed.h"
#include "settings.h"
#include "logging.h"
#include "esp_utils.h"
#include "ble_sensor.h"   // arbitration guard below; safe to include unconditionally
#include "room_avg.h"
#include "link_units.h"

#include <cstring>
#include <cmath>
#include <atomic>
#include <algorithm>
#include <cstdint>

static const char *TAG = "link_sensor";

// ══════════════════════════════════════════════════════════════════════════════
// State
//
// Writers run on the main task: a DIAL_SENSOR frame is queued from the
// ESP-NOW receive callback (on_recv() in espnow_link.cpp, WiFi/ESP-NOW task)
// into an SPSC ring, and EspnowLink::loop() — called from main.cpp on the
// main task, same as LinkSensor::loop() — drains it into feed() via
// h_room_sensor() and calls forget() for a forgotten dial. Readers are not
// all on the main task: the web state push also runs on the httpd task
// (command replies, a newly connected client) and reads the table through
// readings() and the accessors. s_mux guards s_dials for those readers; the
// feed state machine below stays main-task-only.
// ══════════════════════════════════════════════════════════════════════════════

// ── Per-dial readings (guarded by s_mux) ───────────────────────────────────
// One entry per dial, keyed by MAC, so a frame from one dial can never move or
// clear another dial's reading. Only bonded dials reach feed(), and forgetting
// a dial drops its entry (forget()), so the table tracks the bond table.
struct Entry {
    bool     used;
    uint8_t  mac[6];
    bool     hasSensor;   // this dial's latest SL2_DSF_HAS_SENSOR
    int16_t  tempCc;      // centi-C; SL2_CC_NA = no reading yet
    uint16_t humCc;       // SL2_HUM_CC_NA = never reported
    uint64_t lastUpdate;  // uptime_ms64 of the last VALID temp_cc; 0 = none
    uint64_t lastHeard;   // uptime_ms64 of the last frame of any kind
};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static Entry s_dials[SL2_MAX_DIALS];

// ── Feed state machine + deferred clear (main task only, except the atomic) ─
static room_feed_t       s_feed;
static uint8_t           s_feedMac[6] = {0}; // pinned dial of the last feed decision
// Deferred clearRemoteTemperature. INVARIANT: a pending clear and a staged
// send are mutually exclusive — SEND always clears this flag (see loop()).
// room_feed_step() promises at most one "owed" action at a time, so without
// that reset a CLEAR raised while disconnected, followed by a SEND (source
// reselected / reading recovered, still disconnected), would still fire the
// stale clear once the link comes back — overwriting the just-staged fresh
// value with NAN. Do not drop the reset to "simplify" this.
static std::atomic<bool> s_pendingClear{false};

static uint32_t staleMs() {
    return (uint32_t)settings.get().roomStaleTimeoutS * 1000;
}

// The selection's pinned dial, or false when nothing pins one.
static bool pinnedMac(uint8_t out[6]) {
    return sl2_room_source_id_mac(settings.get().roomSourceId,
                                  SL2_ROOM_SOURCE_NS_LINK, out);
}

static Entry *findLocked(const uint8_t mac[6]) {
    for (Entry &e : s_dials)
        if (e.used && memcmp(e.mac, mac, sizeof e.mac) == 0) return &e;
    return nullptr;
}

// A dial new to the table takes a free entry, else the one heard from least
// recently: a full table means a forgotten dial's entry outlived its bond.
static Entry *claimLocked(const uint8_t mac[6]) {
    if (Entry *e = findLocked(mac)) return e;
    Entry *victim = &s_dials[0];
    for (Entry &e : s_dials) {
        if (!e.used) { victim = &e; break; }
        if (e.lastHeard < victim->lastHeard) victim = &e;
    }
    *victim = Entry{};
    victim->used = true;
    victim->tempCc = SL2_CC_NA;
    victim->humCc = SL2_HUM_CC_NA;
    memcpy(victim->mac, mac, sizeof victim->mac);
    return victim;
}

static int32_t meanRounded(int32_t sum, int32_t n) {
    return (sum >= 0 ? sum + n / 2 : sum - n / 2) / n;
}

// What the scalar accessors and the single-mode feed see.
struct View {
    int16_t  tempCc;      // SL2_CC_NA if none
    uint16_t humCc;       // SL2_HUM_CC_NA if none
    uint64_t lastUpdate;  // 0 if none
};

// A pinned selection (pin != nullptr) sees its own dial only; anything else
// sees the summary documented in link_sensor.h. Call while holding s_mux so
// each reading and the MAC it belongs to stay together.
static View viewLocked(const uint8_t *pin, uint64_t now, uint32_t stale) {
    View v = {SL2_CC_NA, SL2_HUM_CC_NA, 0};
    if (pin) {
        if (const Entry *e = findLocked(pin)) v = {e->tempCc, e->humCc, e->lastUpdate};
        return v;
    }
    const Entry *newest = nullptr;
    int32_t tSum = 0, hSum = 0, tN = 0, hN = 0;
    for (const Entry &e : s_dials) {
        if (!e.used || e.lastUpdate == 0) continue;
        if (!newest || e.lastUpdate > newest->lastUpdate) newest = &e;
        if (elapsed_ms32(e.lastUpdate, now) >= stale) continue;
        tSum += e.tempCc;
        tN++;
        if (e.humCc != SL2_HUM_CC_NA) { hSum += e.humCc; hN++; }
    }
    if (!newest) return v;
    v.lastUpdate = newest->lastUpdate;
    v.tempCc = tN ? (int16_t)meanRounded(tSum, tN) : newest->tempCc;
    v.humCc  = hN ? (uint16_t)meanRounded(hSum, hN)
                  : (tN ? (uint16_t)SL2_HUM_CC_NA : newest->humCc);
    return v;
}

// Check the pin on every read, not just at ingest: the web UI and Link both
// write the selection, and an offline replacement may never send a packet.
// pinOut receives the pin this view was taken for (all-zero when unpinned).
static View selectedView(uint8_t *pinOut = nullptr) {
    uint8_t pin[6];
    const bool pinned = pinnedMac(pin);
    if (!pinned) memset(pin, 0, sizeof pin);
    if (pinOut) memcpy(pinOut, pin, sizeof pin);
    const uint64_t now = uptime_ms64();
    const uint32_t stale = staleMs();
    taskENTER_CRITICAL(&s_mux);
    const View v = viewLocked(pinned ? pin : nullptr, now, stale);
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

// ══════════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════════

void LinkSensor::feed(const uint8_t mac[6], const struct sl2_dial_sensor_pkt *p) {
    if (!mac || !p) return;
    // Every dial feeds its own entry whatever the selection, so moving the
    // pin or switching to Link later finds live state rather than a cold
    // start. Freshness follows the temperature, not the flags — a dial that
    // has sensing hardware but no reading yet (or a battery/hum-only frame,
    // if one ever exists) must not keep the stale watchdog from firing.
    const uint64_t now = uptime_ms64();
    taskENTER_CRITICAL(&s_mux);
    Entry *e = claimLocked(mac);
    e->lastHeard = now;
    e->hasSensor = (p->flags & SL2_DSF_HAS_SENSOR) != 0;
    if (p->temp_cc != SL2_CC_NA) {
        e->tempCc     = p->temp_cc;
        e->lastUpdate = now;
    }
    if (p->hum_cc != SL2_HUM_CC_NA) e->humCc = p->hum_cc;
    taskEXIT_CRITICAL(&s_mux);
}

void LinkSensor::forget(const uint8_t mac[6]) {
    taskENTER_CRITICAL(&s_mux);
    for (Entry &e : s_dials)
        if (e.used && (!mac || memcmp(e.mac, mac, sizeof e.mac) == 0)) e = Entry{};
    taskEXIT_CRITICAL(&s_mux);
}

void LinkSensor::loop(CN105Controller &cn105) {
    // Deferred clearRemoteTemperature — kept pending until the CN105 link can
    // actually carry it, so a clear issued while disconnected isn't lost.
    // Symmetric guard to BleSensor's: skip if BLE has since become the
    // selected AND active source — it already owns the register (or is about
    // to, same tick), so clearing here would just stomp its value for up to
    // its own 20s keepalive window. In a build with no BLE, nothing can have
    // taken over, so the clear always proceeds.
    if (s_pendingClear.load() && cn105.isConnected()) {
#ifdef BLE_ENABLE
        // feedSlot() is -1 unless a BLE slot is the selected single-mode
        // source; isActive range-checks, so this is false in every other mode.
        bool handedToBle = BleSensor::isActive(BleSensor::feedSlot());
#else
        bool handedToBle = false;
#endif
        // A switch into Average mode hands the register to the blend the same
        // way — don't stomp its value with this stale clear.
        bool handedToAvg = RoomAvg::isFeeding();
        if (!handedToBle && !handedToAvg) cn105.clearRemoteTemperature();
        s_pendingClear.store(false);
    }

    uint8_t owner[6];   // the pinned dial; all-zero when the summary feeds
    const View v = selectedView(owner);
    const uint64_t now = uptime_ms64();
    const int16_t  tempCc  = v.tempCc;
    const uint64_t lastUpd = v.lastUpdate;
    if (memcmp(s_feedMac, owner, sizeof s_feedMac) != 0) {
        memcpy(s_feedMac, owner, sizeof s_feedMac);
        // A fresh replacement owns a new feeding episode, even if its
        // temperature happens to equal the previous source's value.
        s_feed.have_sent = false;
    }

    // The wire is centi as of proto v3; everything from here down — the change
    // grid, the offsets, the CN105 wire — is deci, so convert once, here.
    // Calibration offset applies in Single mode too, not just in the Average
    // blend. Both are tenths °C, so this stays in integer space — and folding
    // it in before room_feed_step() means the 0.5 °C change grid and the
    // keepalive both track the corrected value.
    int16_t tempDc = (int16_t)std::clamp((int)link_cc_to_dc(tempCc) +
                                             room_offset_dc(settings.get(), ROOM_MEMBER_LINK),
                                         (int)INT16_MIN, (int)INT16_MAX);

    bool     selected    = (settings.get().roomMode == 0) &&
                           (settings.get().roomSingle == ROOM_MEMBER_LINK);
    bool     haveReading = (lastUpd != 0);
    uint32_t age         = haveReading ? elapsed_ms32(lastUpd, now) : 0;
    room_feed_action_t action = room_feed_step(&s_feed, selected, haveReading,
                                                tempDc, age, (uint32_t)now, staleMs());
    switch (action) {
    case ROOM_FEED_SEND:
        // A fresh send supersedes any clear still waiting for the link to
        // come back — see the invariant note on s_pendingClear.
        s_pendingClear.store(false);
        cn105.sendRemoteTemperature(tempDc / 10.0f);
        break;
    case ROOM_FEED_CLEAR:
        // room_feed_step only ever returns CLEAR once per feeding episode, so
        // this fires exactly once — safe to warn unconditionally here.
        LOG_WARN("Link sensor %s — reverting to internal thermistor",
                 selected ? "stale" : "deselected");
        s_pendingClear.store(true);
        break;
    case ROOM_FEED_NONE:
    default:
        break;
    }
}

int LinkSensor::readings(Reading out[SL2_MAX_DIALS]) {
    Entry copy[SL2_MAX_DIALS];
    const uint64_t now = uptime_ms64();
    const uint32_t stale = staleMs();
    taskENTER_CRITICAL(&s_mux);
    memcpy(copy, s_dials, sizeof copy);
    taskEXIT_CRITICAL(&s_mux);
    int n = 0;
    for (const Entry &e : copy) {
        if (!e.used) continue;
        Reading &r = out[n++];
        const bool have = e.lastUpdate != 0;
        memcpy(r.mac, e.mac, sizeof r.mac);
        r.hasSensor   = e.hasSensor;
        r.temperature = have ? e.tempCc / 100.0f : NAN;
        r.humidity    = e.humCc == SL2_HUM_CC_NA ? NAN : e.humCc / 100.0f;
        r.ageMs       = have ? elapsed_ms32(e.lastUpdate, now) : UINT32_MAX;
        r.fresh       = have && r.ageMs < stale;
    }
    return n;
}

bool LinkSensor::hasSensor() {
    // Global hardware capability, also used to permit changing the pin.
    // Freshness/status below belong to the selected reading; tying this gate
    // to an offline pin would prevent selecting another bonded dial.
    bool any = false;
    taskENTER_CRITICAL(&s_mux);
    for (const Entry &e : s_dials) any = any || (e.used && e.hasSensor);
    taskEXIT_CRITICAL(&s_mux);
    return any;
}

bool LinkSensor::isActive() {
    uint64_t lu = selectedView().lastUpdate;
    return lu > 0 && elapsed_ms32(lu, uptime_ms64()) < staleMs();
}

bool LinkSensor::isStale() {
    uint64_t lu = selectedView().lastUpdate;
    return lu > 0 && elapsed_ms32(lu, uptime_ms64()) >= staleMs();
}

float LinkSensor::temperature() {
    int16_t cc = selectedView().tempCc;
    return cc == SL2_CC_NA ? NAN : cc / 100.0f;
}

float LinkSensor::humidity() {
    uint16_t cc = selectedView().humCc;
    return cc == SL2_HUM_CC_NA ? NAN : cc / 100.0f;
}

uint32_t LinkSensor::lastUpdateAge() {
    uint64_t lu = selectedView().lastUpdate;
    return lu == 0 ? UINT32_MAX : elapsed_ms32(lu, uptime_ms64());
}

uint8_t LinkSensor::status() {
    uint64_t lu = selectedView().lastUpdate;
    if (lu == 0) return SL2_ROOMST_UNAVAILABLE;
    return elapsed_ms32(lu, uptime_ms64()) < staleMs() ? SL2_ROOMST_OK : SL2_ROOMST_STALE;
}
