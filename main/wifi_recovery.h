#pragma once

#include <stdint.h>
#include <stddef.h>
#include "board_profile.h"

// Timeout before enabling AP fallback
constexpr uint32_t WIFI_RECOVERY_TIMEOUT_CHANGE = 120000;  // 2 min (after credential change)
constexpr uint32_t WIFI_RECOVERY_TIMEOUT_NORMAL = 300000;  // 5 min (after normal disconnect)

// Delay before disabling AP after WiFi reconnects (lets recovery page confirm)
constexpr uint32_t WIFI_AP_LINGER_MS = 6000;  // 6 seconds (recovery page polls every 3s)

// Change-network window: the dial's WIFI_SETUP raises the AP while the STA is
// still connected; if nobody reconfigures, self-close after this long. (A real
// reprovision drops the STA, which cancels this and runs the normal recovery
// flow.) Each WIFI_SETUP re-send re-arms the window.
constexpr uint32_t WIFI_SETUP_WINDOW_MS = 600000;  // 10 minutes

// Button long-press duration for WiFi reset
constexpr uint32_t WIFI_RESET_BUTTON_HOLD_MS = 10000;  // 10 seconds
// Button medium-press (release before 10s) opens Link pairing
constexpr uint32_t PAIR_BUTTON_HOLD_MS       = 2000;   // 2 seconds
constexpr int8_t   WIFI_RESET_BUTTON_PIN = PIN_BUTTON;  // From board profile (-1 = no button)

class WifiRecovery {
public:
    void begin(const char *apName, const char *displayName);    // Initialize: store AP name, configure button GPIO
    void loop();                       // Call every main-loop iteration: samples the button each
                                       // call (2 s / 10 s hold thresholds need ~10 ms sampling);
                                       // WiFi/AP checks are rate-limited to 1 Hz internally
    bool isAPActive() const { return _apActive; }
    uint32_t buttonHeldMs() const;     // 0 = button not pressed; else ms held so far
    void setChangePending(bool pending); // Set/clear the NVS flag
    void activateNow();                  // Immediately enable fallback AP (no timeout)
    void beginChangeWindow();            // Dial-initiated: AP now + bounded auto-close while STA stays up
    void noteReprovision();              // Portal applied new credentials — the change window may now
                                         // close on join success (see loop()'s level-based close)
    bool wifiChangeFailed() const;       // SL2 wifi_err: last credential change failed —
                                         // trial-connect reverted (WifiManager latch) or the
                                         // change-recovery clock expired without a join
    void getCachedSSID(char *buf, size_t bufLen) const; // Return cached SSID (no NVS I/O)
    uint32_t getWifiUptimeSeconds() const;

private:
    void checkButton();
    void enableFallbackAP();
    void disableFallbackAP();
    void refreshCachedSSID();          // Re-read SSID from WifiManager NVS into _cachedSSID
    static uint32_t safeUptimeMs();    // uptime_ms() that never returns 0 (sentinel avoidance)

    char     _apName[32] = "";
    char     _displayName[32] = "";
    bool     _apActive = false;
    bool     _wasConnected = false;      // Track previous WiFi state
    uint32_t _disconnectedSince = 0;     // uptime_ms() when WiFi was lost (0 = connected)
    uint32_t _wifiConnectedSince = 0;    // uptime_ms() when WiFi connected (0 = not connected)
    uint32_t _apShutdownAt = 0;          // uptime_ms() when AP should be disabled (0 = no pending shutdown)
    uint32_t _lastWifiCheck = 0;         // uptime_ms() of last 1 Hz WiFi/AP check
    uint32_t _buttonPressStart = 0;      // uptime_ms() when button was first pressed (0 = not pressed)
    bool     _buttonTriggered = false;   // Prevent repeat triggers
    bool     _changeWindow = false;      // Dial-initiated change window open (AP up over a live STA).
                                         // While open WITHOUT a reprovision, STA blips (the portal's
                                         // scan, beacon loss) auto-rejoin the OLD network — those
                                         // edges must not clear the pending flag, cancel the window
                                         // deadline, or linger-close the AP (on-device round 2)
    bool     _reprovisioned = false;     // noteReprovision() seen since the window opened
    uint32_t _changeAt = 0;              // uptime_ms() when the current credential change began
                                         // (0 = none active); every change funnels through
                                         // setChangePending(true), which stamps this
    bool     _changeFailed = false;      // latched once a change exceeds WIFI_RECOVERY_TIMEOUT_CHANGE
                                         // without joining; cleared on join or the next change
    char     _cachedSSID[33] = "";       // Cached SSID to avoid NVS reads on every status poll
};

extern WifiRecovery wifiRecovery;
