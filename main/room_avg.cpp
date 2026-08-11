#include "room_avg.h"
#include "room_feed.h"
#include "settings.h"
#include "link_sensor.h"
#include "ble_config.h"
#ifdef BLE_ENABLE
#include "ble_sensor.h"
#endif
#include "logging.h"
#include "esp_utils.h"

#include <cmath>
#include <algorithm>
#include <atomic>

static const char *TAG = "room_avg";

// ── State ───────────────────────────────────────────────────────────────────
// loop() runs on the main task; the snapshot is read from the httpd task
// (state push), so it is copied under s_mux. Everything else is main-task-only
// except the deferred-clear atomic, same discipline as link_sensor.cpp.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static RoomAvg::Status s_status = {};

// Feed state machine + deferred clear — the exact pattern (and invariant)
// documented on link_sensor.cpp's s_pendingClear: a SEND always cancels a
// clear still waiting for the CN105 link to come back.
static room_feed_t       s_feed;
static std::atomic<bool> s_pendingClear{false};

void RoomAvg::loop(CN105Controller &cn105) {
    const DeviceSettings &st = settings.get();
    const uint32_t staleMs = (uint32_t)st.roomStaleTimeoutS * 1000;

    // Deferred clearRemoteTemperature — held until the CN105 link can carry
    // it. Skip if a single-mode source has since become selected AND active
    // (it owns the register now); symmetric to the guards in ble_sensor.cpp
    // and link_sensor.cpp.
    if (s_pendingClear.load() && cn105.isConnected()) {
        bool handedToSingle = (st.roomMode == 0 && st.roomSingle == ROOM_MEMBER_LINK)
                                  ? LinkSensor::isActive()
                                  : false;
#ifdef BLE_ENABLE
        handedToSingle = handedToSingle || BleSensor::isActive(BleSensor::feedSlot());
#endif
        if (!handedToSingle) cn105.clearRemoteTemperature();
        s_pendingClear.store(false);
    }

    Status ns = {};
    const bool averaging = (st.roomMode == 1);
    ns.effective = NAN;
    ns.effAgeMs  = UINT32_MAX;

    float sum = 0, mn = 0, mx = 0;
    int   n   = 0;
    auto contribute = [&](int bit, float t, uint32_t ageMs) {
        t += st.roomOffsets[bit] / 10.0f;
        sum += t;
        mn = n ? std::min(mn, t) : t;
        mx = n ? std::max(mx, t) : t;
        n++;
        ns.contributors |= (uint8_t)(1u << bit);
        ns.effAgeMs = std::min(ns.effAgeMs, ageMs);
    };

    if (averaging) {
        // The internal bit is ignored: internal never blends with a live
        // remote (CN105 echoes the fed value back, so the thermistor is
        // unobservable), and with no remote live the fallback below uses it
        // anyway — so membership carries no meaning. The UI doesn't offer the
        // checkbox; a stored bit (old write / stray client) changes nothing.
        const uint8_t members = st.roomMembers & (uint8_t)~(1u << ROOM_MEMBER_INTERNAL);

        if (members & (1u << ROOM_MEMBER_LINK)) {
            float t = LinkSensor::temperature();
            if (LinkSensor::isActive() && !std::isnan(t))
                contribute(ROOM_MEMBER_LINK, t, LinkSensor::lastUpdateAge());
            else
                ns.exclStale |= (1u << ROOM_MEMBER_LINK);
        }

#ifdef BLE_ENABLE
        for (int i = 0; i < ROOM_MAX_BLE_SENSORS; i++) {
            uint8_t bit = (uint8_t)(1u << (ROOM_MEMBER_BLE0 + i));
            if (!(members & bit)) continue;
            if (!st.bleEnabled) { ns.exclOff |= bit; continue; }
            float t = BleSensor::temperature(i);
            if (BleSensor::isActive(i) && !std::isnan(t))
                contribute(ROOM_MEMBER_BLE0 + i, t, BleSensor::lastUpdateAge(i));
            else
                ns.exclStale |= bit;
        }
#endif

        if (n > 0) {
            ns.effective = sum / n;
            ns.spread    = mx - mn;
        }
        // Remote members are checked but none is live -> the pump falls back
        // to its internal thermistor; the selection is preserved.
        ns.fallback = (n == 0) && (ns.exclStale | ns.exclOff) != 0;
    }

    // Feed decision — the same room_feed change/keepalive/rate limiting as
    // the single-source paths (0.5 °C grid via ROOM_FEED_CHANGE_DC). age 0:
    // staleness was already applied per-member above.
    bool haveBlend = averaging && n > 0;
    int16_t dc = haveBlend ? (int16_t)lroundf(ns.effective * 10.0f) : 0;
    switch (room_feed_step(&s_feed, haveBlend, haveBlend, dc, 0, uptime_ms(), staleMs)) {
    case ROOM_FEED_SEND:
        s_pendingClear.store(false);   // invariant: a send supersedes a queued clear
        cn105.sendRemoteTemperature(dc / 10.0f);
        break;
    case ROOM_FEED_CLEAR:
        LOG_WARN("Average %s — reverting to internal thermistor",
                 averaging ? "has no live member" : "deselected");
        s_pendingClear.store(true);
        break;
    case ROOM_FEED_NONE:
    default:
        break;
    }
    ns.feeding = s_feed.feeding;
    if (!ns.feeding) ns.effective = NAN;

    taskENTER_CRITICAL(&s_mux);
    s_status = ns;
    taskEXIT_CRITICAL(&s_mux);
}

RoomAvg::Status RoomAvg::status() {
    taskENTER_CRITICAL(&s_mux);
    Status s = s_status;
    taskEXIT_CRITICAL(&s_mux);
    return s;
}

bool RoomAvg::isFeeding() {
    return status().feeding;
}
