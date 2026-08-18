// Host tests for the LED priority policy (sled_policy.h), plus a source-level
// check that the driver is still wired into it (see hold_blocking_wiring()).
#include "sled_policy.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// A healthy, fully-up device: everything connected, nothing in progress.
static SledInputs healthy() {
    SledInputs in = {};
    in.webUIStarted   = true;
    in.cn105Connected = true;
    in.wifiConnected  = true;
    in.wifiOnRgb      = true;
    return in;
}

// StatusLED itself needs ESP-IDF headers, so holdBlocking() cannot be run
// here — and dropping either of its two load-bearing details would leave every
// suite green. Pin them by reading that one function's source: it must apply
// its state with blocking=true (else an armed requestHold() transient replaces
// the terminal indication), and it must end back in SLED_OFF (else _state
// keeps a terminal value the dark strip no longer shows, and a second call
// with that same state hits applyState()'s early-out and paints nothing).
static void hold_blocking_wiring() {
    std::ifstream f("../../main/status_led.cpp");  // run.sh cd's to this dir
    assert(f.is_open() && "run this via test/sled_policy/run.sh");
    const std::string src((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    const std::string::size_type b = src.find("void StatusLED::holdBlocking(");
    assert(b != std::string::npos);
    const std::string::size_type e = src.find("\n}\n", b);
    assert(e != std::string::npos);
    const std::string body = src.substr(b, e - b);
    assert(body.find("applyState(state, true)") != std::string::npos);
    assert(body.find("applyState(SLED_OFF, true)") != std::string::npos);
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
    // over one but leaves it armed. No other policy state may override a hold
    // — an armed transient is what the user just asked to see.
    assert(sled_hold_action(SLED_OTA, false) == SLED_HOLD_CANCEL);
    assert(sled_hold_action(SLED_BTN_WIPE, false) == SLED_HOLD_BYPASS);

    for (int s = SLED_OFF; s <= SLED_BTN_WIPE; s++) {
        LEDState st = (LEDState)s;
        // A blocking terminal indication (holdBlocking, driven from the main
        // task right before esp_restart()) always wins, whatever the state,
        // and always by cancelling — nothing resumes after a restart.
        assert(sled_hold_action(st, true) == SLED_HOLD_CANCEL);
        // Non-blocking behavior is exactly the two exemptions above: every
        // other state still defers to the hold.
        if (st == SLED_OTA || st == SLED_BTN_WIPE) continue;
        assert(sled_hold_action(st, false) == SLED_HOLD_SUBSTITUTE);
    }

    // The case that motivated the blocking flag: SLED_UNPAIR has no
    // state-level exemption, so only the blocking path keeps the unpair
    // confirmation from being replaced by a stale identify/verdict transient.
    assert(sled_hold_action(SLED_UNPAIR, false) == SLED_HOLD_SUBSTITUTE);
    assert(sled_hold_action(SLED_UNPAIR, true) == SLED_HOLD_CANCEL);

    hold_blocking_wiring();

    printf("sled_policy: all tests passed\n");
    return 0;
}
