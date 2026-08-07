#pragma once
// Pure LED priority policy — NO ESP-IDF includes; host-testable
// (test/sled_policy). The main loop gathers SledInputs once per tick and
// drives StatusLED with the result; requestHold() overrides still apply
// inside StatusLED::setState().

#include <stdint.h>

// RGB LED states (WS2812). Vocabulary: color = subsystem, motion = activity.
// Pulse = working/waiting (expected), solid = steady state/fault,
// fast blink = attention / momentary result.
enum LEDState {
    SLED_OFF,                // Healthy — LED dark
    SLED_BOOT,               // Power-on until web UI up — white solid
    SLED_CN105_DISCONNECTED, // CN105 UART lost — red solid
    SLED_WIFI_DISCONNECTED,  // WiFi lost — blue solid (WIFI_ON_RGB boards only)
    SLED_ERROR_CODE,         // Heat pump error code — red fast blink (persistent)
    SLED_OTA,                // Firmware upload — white slow pulse
    SLED_PAIR_LISTEN,        // Link pairing window open — purple slow pulse
    SLED_RESULT_OK,          // Success transient (pair OK / WiFi joined) — green solid, held by caller
    SLED_RESULT_FAIL,        // Fail transient (pair fail / creds rejected) — red fast blink, held by caller
    SLED_UNPAIR,             // Link forgotten — orange fast blink (held before restart)
    SLED_PORTAL,             // Setup hotspot open — blue slow pulse
    SLED_WIFI_TRIAL,         // Trying submitted WiFi credentials — blue fast blink
    SLED_SAFE_MODE,          // Crash-loop safe mode — yellow slow pulse
    SLED_IDENTIFY,           // Identify ("this one!") — white fast blink, held by caller
    SLED_BTN_PAIR,           // Button held >=2 s: release opens/forgets Link — purple solid
    SLED_BTN_WIPE            // Button held >=7 s: WiFi erase at 10 s — red fast blink
};

// Button feedback tiers. SLED_BTN_PAIR_TIER_MS must equal
// PAIR_BUTTON_HOLD_MS (wifi_recovery.h) — static_assert in main.cpp.
constexpr uint32_t SLED_BTN_PAIR_TIER_MS = 2000;
constexpr uint32_t SLED_BTN_WIPE_WARN_MS = 7000;   // warn before the 10 s erase

struct SledInputs {
    uint32_t buttonHeldMs;      // 0 = not pressed (wifiRecovery.buttonHeldMs())
    bool     pairActionAllowed; // releasing at the 2 s tier would act (ESPNOW on, no OTA)
    bool     otaActive;         // firmware upload in progress (webota_active())
    bool     pairingActive;     // Link pairing window open
    bool     wifiTrialActive;   // submitted credentials being tried
    bool     safeMode;          // crash-loop safe mode session
    bool     portalActive;      // captive-portal AP up
    bool     webUIStarted;      // boot phase over
    bool     hpError;           // heat pump reporting an error code
    bool     cn105Connected;
    bool     wifiConnected;
    bool     wifiOnRgb;         // board shows WiFi loss on the RGB (no mono blue LED)
};

// ── Hold precedence ─────────────────────────────────────────────────────────
// requestHold() lets another task pin a transient (green success, white
// identify) over whatever the policy picks. Two states outrank a hold, for
// different reasons — both answered here, next to the priority chain, rather
// than as name comparisons buried in the driver.

// A firmware upload supersedes any pending transient outright: the hold is
// cancelled, not deferred (the upload outlasts every hold duration anyway).
inline bool sled_cancels_hold(LEDState state) {
    return state == SLED_OTA;
}

// The wipe warning must be visible the instant it applies — a green/white
// transient may never cover it while the 10 s erase clock runs — but it is
// momentary, so the hold resumes on release rather than being discarded.
inline bool sled_bypasses_hold(LEDState state) {
    return state == SLED_BTN_WIPE;
}

inline LEDState sled_evaluate(const SledInputs& in) {
    if (in.buttonHeldMs >= SLED_BTN_WIPE_WARN_MS) return SLED_BTN_WIPE;
    if (in.buttonHeldMs >= SLED_BTN_PAIR_TIER_MS && in.pairActionAllowed) return SLED_BTN_PAIR;
    if (in.otaActive)          return SLED_OTA;
    if (in.pairingActive)      return SLED_PAIR_LISTEN;
    if (in.wifiTrialActive)    return SLED_WIFI_TRIAL;
    if (in.safeMode)           return SLED_SAFE_MODE;
    if (in.portalActive)       return SLED_PORTAL;
    if (!in.webUIStarted)      return SLED_BOOT;
    if (in.hpError)            return SLED_ERROR_CODE;
    if (!in.cn105Connected)    return SLED_CN105_DISCONNECTED;
    if (in.wifiOnRgb && !in.wifiConnected) return SLED_WIFI_DISCONNECTED;
    return SLED_OFF;
}
