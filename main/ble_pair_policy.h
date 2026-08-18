#pragma once
// Pure proximity-pairing accept rule — NO ESP-IDF includes (host-testable,
// test/ble_pair_policy). Fed advertisement observations, handed a read-only
// view of the configured slots, returns a verdict. Knows nothing about NimBLE,
// NVS, settings, or how the winner is named and stored — that is ble_pair.cpp.
//
// The rule is RELATIVE, not an absolute dBm target: sensor TX power spans
// roughly 0 dBm (Govee) to -20 dBm (some BTHome builds), and the unit is
// usually mounted inside the heat pump's chassis, so absolute path loss is not
// predictable. See the design doc for the derivation.

#include <stdint.h>
#include <string.h>
#include <strings.h>   // strncasecmp

// Mirrors ROOM_MAX_BLE_SENSORS; settings.h pulls in <nvs_flash.h> and cannot be
// included here. ble_pair.cpp static_asserts the two are equal.
constexpr int      BLEPAIR_MAX_SLOTS      = 4;

constexpr int      BLEPAIR_MAX_CANDIDATES = 12;
constexpr int      BLEPAIR_RSSI_RING      = 8;
constexpr int      BLEPAIR_MIN_SAMPLES    = 3;
constexpr int8_t   BLEPAIR_FLOOR_DBM      = -55;   // PROVISIONAL — bench-calibrate
constexpr int8_t   BLEPAIR_MARGIN_DB      = 10;
constexpr int8_t   BLEPAIR_RISE_DB        = 15;    // PROVISIONAL — bench-calibrate
constexpr uint32_t BLEPAIR_WINDOW_MS      = 45000;
constexpr uint32_t BLEPAIR_EARLY_MS       = 15000;
constexpr uint32_t BLEPAIR_BASELINE_MS    = 10000;
constexpr uint32_t BLEPAIR_DEAD_UPTIME_MS = 15u * 60u * 1000u;

// The rise test compares a candidate against a baseline frozen from the
// window's OWN first BLEPAIR_BASELINE_MS (see freezeBaselines). An accept must
// therefore never be reachable before that freeze, or a configured sensor
// would be judged against no baseline at all.
static_assert(BLEPAIR_BASELINE_MS < BLEPAIR_EARLY_MS,
              "baselines must freeze before the earliest possible accept");

enum PairResult : uint8_t {
    PR_PENDING,        // keep listening — the window has not closed
    PR_ACCEPT,
    PR_AMBIGUOUS,      // two candidates too close to call
    PR_NO_CANDIDATE,
    PR_SLOTS_FULL,
};

struct PairVerdict {
    PairResult result;
    char       mac[18];
    int        slot;
    int8_t     medianRssi;
    // The chosen slot is a DEAD slot being reclaimed, i.e. it is configured and
    // will be overwritten. Carried explicitly so the commit side can tell this
    // apart from "a free slot that someone filled mid-window" — the two are
    // indistinguishable from `slot` alone, and treating a reclaim as the latter
    // makes the reclaim path impossible to commit.
    bool       reclaim;
};

struct PairSlotView {
    struct Slot {
        bool   configured;
        char   mac[18];
        bool   seenSinceBoot;   // has produced a reading since it was configured
        int8_t baselineRssi;    // snapshotted when the window opened
        bool   baselineValid;
    } slot[BLEPAIR_MAX_SLOTS];
    int      feedSlot;          // BLE slot currently feeding the pump, -1 if none
    uint32_t uptimeMs;
};

struct PairPolicy {
    void reset() { _n = 0; }

    // Called from the NimBLE host task. Only advertisements that decoded a
    // temperature are admitted — that one condition filters out phones,
    // watches, tags and beacons without needing an allow-list.
    void observe(const char* mac, int rssi, bool tempDecoded) {
        if (!tempDecoded || !mac || !mac[0]) return;
        if (rssi < -127) rssi = -127;
        if (rssi >   20) rssi =   20;
        const int8_t r = (int8_t)rssi;

        for (int i = 0; i < _n; i++) {
            // Bounded to the stored MAC's declared size: a caller-supplied
            // string longer than that must still match the (truncated)
            // stored candidate, or an over-long MAC would spawn a fresh
            // candidate every frame and never accumulate samples.
            if (strncasecmp(_c[i].mac, mac, sizeof(_c[i].mac) - 1) == 0) { push(_c[i], r); return; }
        }
        if (_n < BLEPAIR_MAX_CANDIDATES) { init(_c[_n++], mac, r); return; }

        // Table full: a proximity gate wants the strongest twelve, so evict the
        // weakest only when the newcomer beats it. Ranked by median(), not the
        // most recent sample: frame-to-frame swings of +/-10 dB are routine, so
        // ranking by a single frame would let one deep fade on the sensor the
        // user is actually holding make it look like the weakest entry and
        // evict it.
        int weakest = 0;
        for (int i = 1; i < _n; i++)
            if (median(_c[i]) < median(_c[weakest])) weakest = i;
        if (r > median(_c[weakest])) init(_c[weakest], mac, r);
    }

    // Called from the main task once, BLEPAIR_BASELINE_MS into the window.
    // Every candidate heard so far gets its current median frozen as the
    // baseline the rise test measures against. This is what makes the gate
    // trustworthy: PairSlotView's baseline is one raw frame that may be up to
    // roomStaleTimeoutS (600-3600 s) old, so a stationary sensor whose last
    // stored frame happened to be a deep fade would appear to have "risen".
    // A baseline taken from the window's own opening seconds cannot drift, and
    // the documented flow (press first, THEN approach) means the sensor the
    // user is carrying is still far away while it is being taken.
    void freezeBaselines() {
        for (int i = 0; i < _n; i++) {
            if (_c[i].n == 0) continue;
            _c[i].baseline    = median(_c[i]);
            _c[i].hasBaseline = true;
        }
    }

    // Called from the main task. Returns PR_PENDING while the window is still
    // open and nothing has decisively won; never PENDING once it has closed.
    PairVerdict evaluate(const PairSlotView& v, uint32_t elapsedMs) const {
        PairVerdict out = {PR_PENDING, {0}, -1, 0, false};
        const bool closed = (elapsedMs >= BLEPAIR_WINDOW_MS);

        int    bestIdx = -1, runnerIdx = -1;
        int8_t bestMed = 0,  runnerMed = 0;

        for (int i = 0; i < _n; i++) {
            if (_c[i].n < BLEPAIR_MIN_SAMPLES) continue;
            const int8_t med = median(_c[i]);
            if (med < BLEPAIR_FLOOR_DBM) continue;

            // Every configured sensor with a valid baseline must have RISEN
            // against it to win — the currently-feeding slot included. A
            // wall-mounted sensor sitting still cannot; one you pick up and
            // carry over trivially does. The feed slot is NOT exempted:
            // exempting it (re-accepting it is a no-op) would leave it
            // permanently eligible as a runner-up and let it manufacture
            // PR_AMBIGUOUS against a genuinely held new sensor. The cost is
            // small — holding the already-feeding sensor without a rise now
            // reads PR_NO_CANDIDATE instead of a no-op PR_ACCEPT; neither
            // outcome changes any stored state.
            //
            // Baseline preference: this window's own frozen median first, the
            // slot view's last-frame snapshot only when the candidate was not
            // yet being heard at freeze time (battery just replaced, sensor
            // powered on mid-window). With neither, the sensor is exempt.
            const int s = slotOf(v, _c[i].mac);
            if (s >= 0) {
                int8_t base     = 0;
                bool   haveBase = false;
                if (_c[i].hasBaseline)            { base = _c[i].baseline;         haveBase = true; }
                else if (v.slot[s].baselineValid) { base = v.slot[s].baselineRssi;  haveBase = true; }
                if (haveBase && med < (int)base + BLEPAIR_RISE_DB) continue;
            }

            if (bestIdx < 0 || med > bestMed) {
                runnerIdx = bestIdx; runnerMed = bestMed;
                bestIdx   = i;       bestMed   = med;
            } else if (runnerIdx < 0 || med > runnerMed) {
                runnerIdx = i;       runnerMed = med;
            }
        }

        if (bestIdx < 0) { out.result = closed ? PR_NO_CANDIDATE : PR_PENDING; return out; }

        // The runner-up is drawn from the ELIGIBLE set only. A stationary
        // configured sensor that failed the rise test is known not to be in the
        // user's hand, so letting it manufacture ambiguity would block
        // legitimate re-promotions.
        if (runnerIdx >= 0 && (int)(bestMed - runnerMed) < BLEPAIR_MARGIN_DB) {
            out.result = closed ? PR_AMBIGUOUS : PR_PENDING;
            return out;
        }
        if (!closed && elapsedMs < BLEPAIR_EARLY_MS) return out;   // PR_PENDING

        int s = slotOf(v, _c[bestIdx].mac);
        if (s < 0) {
            for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++)
                if (!v.slot[i].configured) { s = i; break; }
        }
        if (s < 0 && v.uptimeMs >= BLEPAIR_DEAD_UPTIME_MS) {
            // "Dead" is expressed in uptime, never a stored timestamp: with
            // SNTP gone a phone-free device may have no valid wall clock.
            for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++)
                if (i != v.feedSlot && !v.slot[i].seenSinceBoot) {
                    s = i; out.reclaim = true; break;
                }
        }
        if (s < 0) {
            // Not decisive yet — a configured sensor may still rise and win a
            // slot it already owns, so only give up once the window closes.
            out.result = closed ? PR_SLOTS_FULL : PR_PENDING;
            return out;
        }

        out.result     = PR_ACCEPT;
        out.slot       = s;
        out.medianRssi = bestMed;
        strncpy(out.mac, _c[bestIdx].mac, sizeof(out.mac) - 1);
        out.mac[sizeof(out.mac) - 1] = '\0';
        return out;
    }

private:
    struct Cand {
        char    mac[18];
        int8_t  ring[BLEPAIR_RSSI_RING];
        uint8_t n;   // samples held, capped at BLEPAIR_RSSI_RING
        uint8_t w;   // write cursor
        int8_t  baseline;      // median at BLEPAIR_BASELINE_MS, if hasBaseline
        bool    hasBaseline;   // was this MAC heard before the freeze?
    };

    static void init(Cand& c, const char* mac, int8_t r) {
        memset(&c, 0, sizeof(c));
        strncpy(c.mac, mac, sizeof(c.mac) - 1);
        push(c, r);
    }
    static void push(Cand& c, int8_t r) {
        c.ring[c.w] = r;
        c.w = (uint8_t)((c.w + 1) % BLEPAIR_RSSI_RING);
        if (c.n < BLEPAIR_RSSI_RING) c.n++;
    }
    // Median, not mean: frame-to-frame swings of +/-10 dB are routine and a
    // mean chases them.
    static int8_t median(const Cand& c) {
        int8_t t[BLEPAIR_RSSI_RING];
        for (uint8_t i = 0; i < c.n; i++) t[i] = c.ring[i];
        for (uint8_t i = 1; i < c.n; i++) {
            int8_t k = t[i]; int j = (int)i - 1;
            while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
            t[j + 1] = k;
        }
        return t[c.n / 2];
    }
    // Bounded to sizeof(mac)-1: a slot MAC that (through caller error) lacks
    // a trailing NUL must never be read past its declared 18-byte size.
    static int slotOf(const PairSlotView& v, const char* mac) {
        for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++)
            if (v.slot[i].configured &&
                strncasecmp(v.slot[i].mac, mac, sizeof(v.slot[i].mac) - 1) == 0) return i;
        return -1;
    }

    Cand _c[BLEPAIR_MAX_CANDIDATES] = {};
    int  _n = 0;
};
