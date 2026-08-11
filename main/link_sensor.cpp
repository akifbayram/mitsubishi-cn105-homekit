#include "link_sensor.h"
#include "room_feed.h"
#include "settings.h"
#include "logging.h"
#include "esp_utils.h"
#include "ble_sensor.h"   // arbitration guard below; safe to include unconditionally
#include "room_avg.h"

#include <cstring>
#include <cmath>
#include <atomic>

static const char *TAG = "link_sensor";

// ══════════════════════════════════════════════════════════════════════════════
// State
//
// feed() and loop() both currently run on the main task: a DIAL_SENSOR frame
// is queued from the ESP-NOW receive callback (on_recv() in espnow_link.cpp,
// which runs in the WiFi/ESP-NOW task) into an SPSC ring, and
// EspnowLink::loop() — called from main.cpp on the main task, same as
// LinkSensor::loop() — drains that ring and calls feed() via h_room_sensor().
// The s_mux spinlock below is therefore not load-bearing today; it's kept
// deliberately, same discipline as ble_sensor.cpp's s_mux, so this module
// stays correct if the drain is ever moved off the main task. Everything
// below is main-task-only unless noted.
// ══════════════════════════════════════════════════════════════════════════════

// ── Latest reading (guarded by s_mux) ───────────────────────────────────────
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int16_t  s_tempDc     = SL2_DC_NA;   // deci-C; SL2_DC_NA = no reading yet
static uint8_t  s_humPct     = SL2_HUM_NA;
static uint32_t s_lastUpdate = 0;           // uptime_ms of last VALID temp_dc
static uint8_t  s_mac[6]     = {0};         // MAC of the dial that last fed us
static bool     s_hasSensor  = false;       // latest packet's SL2_DSF_HAS_SENSOR

// ── Feed state machine + deferred clear (main task only, except the atomic) ─
static room_feed_t       s_feed;
// Deferred clearRemoteTemperature. INVARIANT: a pending clear and a staged
// send are mutually exclusive — SEND always clears this flag (see loop()).
// room_feed_step() promises at most one "owed" action at a time, so without
// that reset a CLEAR raised while disconnected, followed by a SEND (source
// reselected / reading recovered, still disconnected), would still fire the
// stale clear once the link comes back — overwriting the just-staged fresh
// value with NAN. Do not drop the reset to "simplify" this.
static std::atomic<bool> s_pendingClear{false};

// ── Spinlock helper ─────────────────────────────────────────────────────────
template<typename T>
static T readLocked(const T& var) {
    taskENTER_CRITICAL(&s_mux);
    T v = var;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

// ══════════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════════

void LinkSensor::feed(const uint8_t mac[6], const struct sl2_dial_sensor_pkt *p) {
    if (!p) return;

    // Freshness follows the temperature, not the flags — a dial that has
    // sensing hardware but no reading yet (or a battery/hum-only frame, if
    // one ever exists) must not keep the stale watchdog from firing.
    bool gotTemp = (p->temp_dc != SL2_DC_NA);

    taskENTER_CRITICAL(&s_mux);
    s_hasSensor = (p->flags & SL2_DSF_HAS_SENSOR) != 0;
    if (gotTemp) {
        s_tempDc     = p->temp_dc;
        s_lastUpdate = uptime_ms();
    }
    if (p->hum_pct != SL2_HUM_NA) s_humPct = p->hum_pct;
    if (mac) memcpy(s_mac, mac, 6);
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

    uint32_t now = uptime_ms();
    int16_t  tempDc;
    uint32_t lastUpd;
    taskENTER_CRITICAL(&s_mux);
    tempDc  = s_tempDc;
    lastUpd = s_lastUpdate;
    taskEXIT_CRITICAL(&s_mux);

    bool     selected    = (settings.get().roomMode == 0) &&
                           (settings.get().roomSingle == ROOM_MEMBER_LINK);
    bool     haveReading = (lastUpd != 0);
    uint32_t age         = haveReading ? (now - lastUpd) : 0;
    uint32_t staleMs      = (uint32_t)settings.get().roomStaleTimeoutS * 1000;

    room_feed_action_t action = room_feed_step(&s_feed, selected, haveReading,
                                                tempDc, age, now, staleMs);
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

bool LinkSensor::hasSensor() {
    return readLocked(s_hasSensor);
}

bool LinkSensor::isActive() {
    uint32_t lu = readLocked(s_lastUpdate);
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    return lu > 0 && (uptime_ms() - lu) < staleMs;
}

bool LinkSensor::isStale() {
    uint32_t lu = readLocked(s_lastUpdate);
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    return lu > 0 && (uptime_ms() - lu) >= staleMs;
}

float LinkSensor::temperature() {
    int16_t dc = readLocked(s_tempDc);
    return dc == SL2_DC_NA ? NAN : dc / 10.0f;
}

float LinkSensor::humidity() {
    uint8_t h = readLocked(s_humPct);
    return h == SL2_HUM_NA ? NAN : (float)h;
}

uint32_t LinkSensor::lastUpdateAge() {
    uint32_t lu = readLocked(s_lastUpdate);
    return lu == 0 ? UINT32_MAX : (uptime_ms() - lu);
}

uint8_t LinkSensor::status() {
    uint32_t lu = readLocked(s_lastUpdate);
    if (lu == 0) return SL2_ROOMST_UNAVAILABLE;
    uint32_t staleMs = (uint32_t)settings.get().roomStaleTimeoutS * 1000;
    return (uptime_ms() - lu) < staleMs ? SL2_ROOMST_OK : SL2_ROOMST_STALE;
}
