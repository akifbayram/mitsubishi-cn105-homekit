// Host tests for the LED priority policy (sled_policy.h).
#include "sled_policy.h"
#include <cassert>
#include <cstdio>

// A healthy, fully-up device: everything connected, nothing in progress.
static SledInputs healthy() {
    SledInputs in = {};
    in.webUIStarted   = true;
    in.cn105Connected = true;
    in.wifiConnected  = true;
    in.wifiOnRgb      = true;
    return in;
}

int main() {
    // Healthy → dark
    assert(sled_evaluate(healthy()) == SLED_OFF);

    // Boot: web UI not started yet → BOOT regardless of subsystem states
    { SledInputs in = healthy(); in.webUIStarted = false; in.cn105Connected = false;
      assert(sled_evaluate(in) == SLED_BOOT); }

    // Bottom rungs: WiFi loss shows only on WIFI_ON_RGB boards
    { SledInputs in = healthy(); in.wifiConnected = false;
      assert(sled_evaluate(in) == SLED_WIFI_DISCONNECTED); }
    { SledInputs in = healthy(); in.wifiConnected = false; in.wifiOnRgb = false;
      assert(sled_evaluate(in) == SLED_OFF); }

    // CN105 loss outranks WiFi loss; HP error outranks CN105 loss
    { SledInputs in = healthy(); in.wifiConnected = false; in.cn105Connected = false;
      assert(sled_evaluate(in) == SLED_CN105_DISCONNECTED); }
    { SledInputs in = healthy(); in.cn105Connected = false; in.hpError = true;
      assert(sled_evaluate(in) == SLED_ERROR_CODE); }

    // Portal outranks heat-pump states (setup context wins)
    { SledInputs in = healthy(); in.portalActive = true; in.cn105Connected = false; in.hpError = true;
      assert(sled_evaluate(in) == SLED_PORTAL); }

    // Safe mode outranks portal (both are active in safe mode)
    { SledInputs in = healthy(); in.portalActive = true; in.safeMode = true;
      assert(sled_evaluate(in) == SLED_SAFE_MODE); }

    // WiFi trial outranks safe mode + portal
    { SledInputs in = healthy(); in.portalActive = true; in.safeMode = true; in.wifiTrialActive = true;
      assert(sled_evaluate(in) == SLED_WIFI_TRIAL); }

    // Pairing listen outranks trial
    { SledInputs in = healthy(); in.wifiTrialActive = true; in.pairingActive = true;
      assert(sled_evaluate(in) == SLED_PAIR_LISTEN); }

    // BLE proximity-pair window sits just under Link pairing: it outranks the
    // WiFi trial, and Link pairing outranks it (the two are mutually exclusive
    // by guard, but the order must still be defined).
    { SledInputs in = healthy(); in.wifiTrialActive = true; in.blePairListening = true;
      assert(sled_evaluate(in) == SLED_BLE_PAIR_LISTEN); }
    { SledInputs in = healthy(); in.blePairListening = true; in.pairingActive = true;
      assert(sled_evaluate(in) == SLED_PAIR_LISTEN); }

    // OTA and the button hold tiers still outrank it
    { SledInputs in = healthy(); in.blePairListening = true; in.otaActive = true;
      assert(sled_evaluate(in) == SLED_OTA); }
    { SledInputs in = healthy(); in.blePairListening = true;
      in.buttonHeldMs = SLED_BTN_WIPE_WARN_MS;
      assert(sled_evaluate(in) == SLED_BTN_WIPE); }

    // OTA outranks pairing listen and everything below
    { SledInputs in = healthy(); in.pairingActive = true; in.otaActive = true;
      assert(sled_evaluate(in) == SLED_OTA); }

    // Button pair tier (>=2 s) outranks OTA-substates below it, but is
    // suppressed when the release action would be ignored (pairActionAllowed=false)
    { SledInputs in = healthy(); in.buttonHeldMs = SLED_BTN_PAIR_TIER_MS; in.pairActionAllowed = true;
      assert(sled_evaluate(in) == SLED_BTN_PAIR); }
    { SledInputs in = healthy(); in.buttonHeldMs = SLED_BTN_PAIR_TIER_MS; in.pairActionAllowed = false;
      in.otaActive = true;
      assert(sled_evaluate(in) == SLED_OTA); }

    // Wipe warning tier (>=7 s) always shows — even during OTA
    { SledInputs in = healthy(); in.buttonHeldMs = SLED_BTN_WIPE_WARN_MS; in.otaActive = true;
      assert(sled_evaluate(in) == SLED_BTN_WIPE); }

    // Wipe warning outranks the pair tier when both thresholds are crossed
    { SledInputs in = healthy(); in.buttonHeldMs = SLED_BTN_WIPE_WARN_MS; in.pairActionAllowed = true;
      assert(sled_evaluate(in) == SLED_BTN_WIPE); }

    // Short hold (<2 s) shows nothing special
    { SledInputs in = healthy(); in.buttonHeldMs = 1999; in.pairActionAllowed = true;
      assert(sled_evaluate(in) == SLED_OFF); }

    // ── Hold precedence ────────────────────────────────────────────────────
    // An OTA supersedes a pending transient outright; the wipe warning shows
    // over one but leaves it armed. Nothing else may override a hold — an
    // armed transient is what the user just asked to see.
    assert(sled_cancels_hold(SLED_OTA));
    assert(!sled_bypasses_hold(SLED_OTA));
    assert(sled_bypasses_hold(SLED_BTN_WIPE));
    assert(!sled_cancels_hold(SLED_BTN_WIPE));
    for (int s = SLED_OFF; s <= SLED_BTN_WIPE; s++) {
        LEDState st = (LEDState)s;
        if (st == SLED_OTA || st == SLED_BTN_WIPE) continue;
        assert(!sled_cancels_hold(st) && !sled_bypasses_hold(st));
    }

    printf("sled_policy: all tests passed\n");
    return 0;
}
