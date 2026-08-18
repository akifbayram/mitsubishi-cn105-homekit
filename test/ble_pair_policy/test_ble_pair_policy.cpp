// Host tests for the BLE proximity-pairing accept rule (ble_pair_policy.h).
#include "ble_pair_policy.h"
#include <cassert>
#include <cstdio>
#include <cstring>

// Nothing configured, device up long enough to reclaim a dead slot.
static PairSlotView emptyView() {
    PairSlotView v = {};
    v.feedSlot = -1;
    v.uptimeMs = BLEPAIR_DEAD_UPTIME_MS + 1;
    return v;
}

static void configure(PairSlotView& v, int idx, const char* mac,
                      bool seen, int8_t baseline, bool baselineValid) {
    v.slot[idx].configured    = true;
    strncpy(v.slot[idx].mac, mac, sizeof(v.slot[idx].mac) - 1);
    v.slot[idx].seenSinceBoot = seen;
    v.slot[idx].baselineRssi  = baseline;
    v.slot[idx].baselineValid = baselineValid;
}

static void feed(PairPolicy& p, const char* mac, int rssi, int n) {
    for (int i = 0; i < n; i++) p.observe(mac, rssi, true);
}

// Past the early-accept floor but before the window closes.
static constexpr uint32_t MID    = BLEPAIR_EARLY_MS + 1000;
static constexpr uint32_t CLOSED = BLEPAIR_WINDOW_MS;

// RSSI test points, all derived from BLEPAIR_FLOOR_DBM so a bench retune of
// the floor can't silently invalidate these eligibility tests.
static constexpr int STRONG   = BLEPAIR_FLOOR_DBM + 25;  // -30, held against the case
static constexpr int MOUNTED  = BLEPAIR_FLOOR_DBM + 10;  // -45, wall-mounted baseline
static constexpr int WEAK     = BLEPAIR_FLOOR_DBM +  1;  // -54, just above the floor
static constexpr int NEAR     = BLEPAIR_FLOOR_DBM + 15;  // -40, paired with an outlier sample
static constexpr int RISEN    = BLEPAIR_FLOOR_DBM + 20;  // -35, no-baseline candidate
static constexpr int OUTLIER  = BLEPAIR_FLOOR_DBM + 50;  // -5, extreme single-sample spike
static constexpr int HELD     = BLEPAIR_FLOOR_DBM + 13;  // -42, 3 dB above MOUNTED, still under margin
static constexpr int STRONGER = BLEPAIR_FLOOR_DBM + 30;  // -25, clearly beats the WEAK filler
static constexpr int FADE     = BLEPAIR_FLOOR_DBM - 20;  // -75, a single deep-fade frame

int main() {
    // A single strong candidate is accepted into slot 0
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, 4);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); assert(d.slot == 0);
      assert(strcmp(d.mac, "AA:BB:CC:DD:EE:01") == 0);
      assert(d.medianRssi == STRONG); }

    // Fewer than BLEPAIR_MIN_SAMPLES frames is not yet decidable
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, BLEPAIR_MIN_SAMPLES - 1);
      assert(p.evaluate(v, MID).result == PR_PENDING);
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    // Below the floor is never a candidate
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", BLEPAIR_FLOOR_DBM - 1, 5);
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    // Two candidates inside the margin stay pending, then go ambiguous
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, 5);
      feed(p, "AA:BB:CC:DD:EE:02", STRONG - (BLEPAIR_MARGIN_DB - 1), 5);
      assert(p.evaluate(v, MID).result == PR_PENDING);
      assert(p.evaluate(v, CLOSED).result == PR_AMBIGUOUS); }

    // Clearing the margin accepts the leader
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, 5);
      feed(p, "AA:BB:CC:DD:EE:02", STRONG - (BLEPAIR_MARGIN_DB + 2), 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT);
      assert(strcmp(d.mac, "AA:BB:CC:DD:EE:01") == 0); }

    // No early accept before BLEPAIR_EARLY_MS, even when unopposed
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, 5);
      assert(p.evaluate(v, BLEPAIR_EARLY_MS - 1).result == PR_PENDING); }

    // A single outlier sample does not move the median
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", NEAR, 4);
      p.observe("AA:BB:CC:DD:EE:01", OUTLIER, true);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); assert(d.medianRssi == NEAR); }

    // A configured, non-feeding sensor sitting still cannot win: no rise
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      v.feedSlot = -1;
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, 5);
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    // The same sensor carried over — a full rise — is re-promoted to its slot
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      v.feedSlot = -1;
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED + BLEPAIR_RISE_DB, 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); assert(d.slot == 0); }

    // A configured sensor with no baseline (stale, battery just replaced) is
    // not blocked by the rise test
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", false, 0, false);
      feed(p, "AA:BB:CC:DD:EE:01", RISEN, 5);
      assert(p.evaluate(v, MID).result == PR_ACCEPT); }

    // The current feed slot is no longer exempt from the rise test: holding
    // it without a rise reads "no candidate", not a no-op "accept"
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      v.feedSlot = 0;
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, 5);
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    // ...but a feed-slot sensor that DOES rise (a genuine re-pair gesture) is
    // still accepted back into its own slot
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      v.feedSlot = 0;
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED + BLEPAIR_RISE_DB, 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); assert(d.slot == 0); }

    // The frozen baseline, not the slot view's stored frame, is what the rise
    // test measures against once the freeze has run. This is the failure it
    // exists to prevent: the view baseline is ONE frame, valid for the whole
    // stale timeout (600-3600 s), and if it happened to be a deep fade then a
    // wall-mounted sensor sitting perfectly still appears to have "risen" and
    // gets silently promoted out of Average mode with no undo.
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, FADE, true);   // stale fade frame
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, 5);
      p.freezeBaselines();
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, BLEPAIR_RSSI_RING);
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    // Same, with no view baseline at all: the freeze supplies one, so a
    // stationary sensor is no longer exempt just because its slot went stale
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, 0, false);
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, 5);
      p.freezeBaselines();
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, BLEPAIR_RSSI_RING);
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    // ...and a sensor actually carried over AFTER the freeze still wins
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, 5);
      p.freezeBaselines();
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED + BLEPAIR_RISE_DB, BLEPAIR_RSSI_RING);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); assert(d.slot == 0); }

    // A sensor with no samples before the freeze gets no frozen baseline and
    // falls back to the view's snapshot — still blocked while stationary
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      p.freezeBaselines();                        // nothing heard yet
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, 5);
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    // With neither baseline (first heard after the freeze, slot never read)
    // the sensor stays exempt, as before
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", false, 0, false);
      p.freezeBaselines();
      feed(p, "AA:BB:CC:DD:EE:01", RISEN, 5);
      assert(p.evaluate(v, MID).result == PR_ACCEPT); }

    // A stationary configured sensor must not manufacture ambiguity for a
    // genuinely held new sensor
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED, 5);   // stationary, ineligible
      feed(p, "AA:BB:CC:DD:EE:02", HELD, 5);      // held, only a few dB stronger
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT);
      assert(strcmp(d.mac, "AA:BB:CC:DD:EE:02") == 0); assert(d.slot == 1); }

    // A new sensor lands in the lowest free slot. uptimeMs is held below the
    // dead-slot threshold so the reclaim path (which does not check
    // `configured`) cannot also land on slot 2 and mask a missing free-slot
    // search.
    { PairPolicy p; PairSlotView v = emptyView();
      v.uptimeMs = 0;
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      configure(v, 1, "AA:BB:CC:DD:EE:02", true, MOUNTED, true);
      feed(p, "AA:BB:CC:DD:EE:09", STRONG, 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); assert(d.slot == 2);
      assert(!d.reclaim); }

    // All slots full and alive → refuse
    { PairPolicy p; PairSlotView v = emptyView();
      for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++) {
          char m[18]; snprintf(m, sizeof(m), "AA:BB:CC:DD:EE:0%d", i);
          configure(v, i, m, true, MOUNTED, true);
      }
      feed(p, "AA:BB:CC:DD:EE:09", STRONG, 5);
      assert(p.evaluate(v, MID).result == PR_PENDING);
      assert(p.evaluate(v, CLOSED).result == PR_SLOTS_FULL); }

    // A dead slot (never seen since boot) is reclaimed, and the verdict says
    // so. The flag is load-bearing, not cosmetic: the chosen slot is by
    // construction still configured, so without an explicit reclaim marker the
    // commit side cannot tell this apart from "a free slot someone filled from
    // the phone mid-window" and refuses every reclaim.
    { PairPolicy p; PairSlotView v = emptyView();
      for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++) {
          char m[18]; snprintf(m, sizeof(m), "AA:BB:CC:DD:EE:0%d", i);
          configure(v, i, m, i != 2, MOUNTED, true);
      }
      feed(p, "AA:BB:CC:DD:EE:09", STRONG, 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); assert(d.slot == 2);
      assert(d.reclaim);
      assert(v.slot[d.slot].configured); }

    // Re-promoting a sensor into the slot it already owns is NOT a reclaim —
    // the commit side must keep its "slot taken by someone else" guard for it
    { PairPolicy p; PairSlotView v = emptyView();
      configure(v, 0, "AA:BB:CC:DD:EE:01", true, MOUNTED, true);
      feed(p, "AA:BB:CC:DD:EE:01", MOUNTED + BLEPAIR_RISE_DB, 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); assert(d.slot == 0);
      assert(!d.reclaim); }

    // Reclaim is refused below the uptime threshold — there is no wall clock,
    // so a freshly rebooted unit cannot tell dead from merely quiet
    { PairPolicy p; PairSlotView v = emptyView();
      v.uptimeMs = BLEPAIR_DEAD_UPTIME_MS - 1;
      for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++) {
          char m[18]; snprintf(m, sizeof(m), "AA:BB:CC:DD:EE:0%d", i);
          configure(v, i, m, i != 2, MOUNTED, true);
      }
      feed(p, "AA:BB:CC:DD:EE:09", STRONG, 5);
      assert(p.evaluate(v, CLOSED).result == PR_SLOTS_FULL); }

    // Reclaim never evicts the slot currently feeding the heat pump
    { PairPolicy p; PairSlotView v = emptyView();
      v.feedSlot = 2;
      for (int i = 0; i < BLEPAIR_MAX_SLOTS; i++) {
          char m[18]; snprintf(m, sizeof(m), "AA:BB:CC:DD:EE:0%d", i);
          configure(v, i, m, i != 2, MOUNTED, true);
      }
      feed(p, "AA:BB:CC:DD:EE:09", STRONG, 5);
      assert(p.evaluate(v, CLOSED).result == PR_SLOTS_FULL); }

    // Frames with no decoded temperature are never admitted
    { PairPolicy p; PairSlotView v = emptyView();
      for (int i = 0; i < 5; i++) p.observe("AA:BB:CC:DD:EE:01", STRONG, false);
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    // MAC matching is case-insensitive: one candidate, not two
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "aa:bb:cc:dd:ee:01", STRONG, 3);
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, 2);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT); }

    // An overflowing table keeps the strongest, so the held sensor still wins
    { PairPolicy p; PairSlotView v = emptyView();
      for (int i = 0; i < BLEPAIR_MAX_CANDIDATES; i++) {
          char m[18]; snprintf(m, sizeof(m), "AA:BB:CC:DD:%02X:00", 0xF0 + i);
          assert(strlen(m) == 17);
          feed(p, m, WEAK, 5);
      }
      feed(p, "AA:BB:CC:DD:EE:09", STRONGER, 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT);
      assert(strcmp(d.mac, "AA:BB:CC:DD:EE:09") == 0); }

    // A weaker newcomer cannot evict anything when the table is full — every
    // sample is dropped, so the incumbent (index 0 in the internal table) is
    // untouched
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, 5);   // the incumbent, clearly strongest
      for (int i = 0; i < BLEPAIR_MAX_CANDIDATES - 1; i++) {
          char m[18]; snprintf(m, sizeof(m), "AA:BB:CC:DD:%02X:00", 0xF0 + i);
          assert(strlen(m) == 17);
          feed(p, m, WEAK, 5);                   // 11 tied, weaker fillers
      }
      // Weaker than every filler already in the table — must not evict.
      feed(p, "AA:BB:CC:DD:EE:09", BLEPAIR_FLOOR_DBM, 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT);
      assert(strcmp(d.mac, "AA:BB:CC:DD:EE:01") == 0); }

    // A strong incumbent (high true median) must not be evicted just because
    // its most recent single frame faded — regression test for ranking
    // eviction by median() rather than the most recent sample
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, BLEPAIR_RSSI_RING);  // fills the ring
      p.observe("AA:BB:CC:DD:EE:01", FADE, true);               // one deep fade
      for (int i = 0; i < BLEPAIR_MAX_CANDIDATES - 1; i++) {
          char m[18]; snprintf(m, sizeof(m), "AA:BB:CC:DD:%02X:00", 0xF0 + i);
          assert(strlen(m) == 17);
          feed(p, m, MOUNTED, 5);                // 11 tied, moderate fillers
      }
      // Stronger than the fillers, weaker than the incumbent's true median —
      // should evict a filler, never the incumbent.
      feed(p, "AA:BB:CC:DD:EE:09", HELD, 5);
      PairVerdict d = p.evaluate(v, MID);
      assert(d.result == PR_ACCEPT);
      assert(strcmp(d.mac, "AA:BB:CC:DD:EE:01") == 0); }

    // reset() clears the table between windows
    { PairPolicy p; PairSlotView v = emptyView();
      feed(p, "AA:BB:CC:DD:EE:01", STRONG, 5);
      p.reset();
      assert(p.evaluate(v, CLOSED).result == PR_NO_CANDIDATE); }

    printf("ble_pair_policy: all tests passed\n");
    return 0;
}
