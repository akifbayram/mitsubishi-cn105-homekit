#pragma once

#include <stdint.h>
#include <stddef.h>
#include "board_profile.h"

// Timeout before enabling AP fallback
constexpr uint32_t WIFI_RECOVERY_TIMEOUT_CHANGE = 120000;  // 2 min (after credential change)
constexpr uint32_t WIFI_RECOVERY_TIMEOUT_NORMAL = 300000;  // 5 min (after normal disconnect)

// Delay before disabling AP after WiFi reconnects (lets recovery page confirm)
constexpr uint32_t WIFI_AP_LINGER_MS = 6000;  // 6 seconds (recovery page polls every 3s)

// Button long-press duration for WiFi reset
constexpr uint32_t WIFI_RESET_BUTTON_HOLD_MS = 10000;  // 10 seconds
// Button medium-press (release before 10s) opens Link pairing
constexpr uint32_t PAIR_BUTTON_HOLD_MS       = 2000;   // 2 seconds
constexpr int8_t   WIFI_RESET_BUTTON_PIN = PIN_BUTTON;  // From board profile (-1 = no button)

class WifiRecovery {
public:
    void begin(const char *apName, const char *displayName);    // Initialize: store AP name, configure button GPIO
    void loop();                       // Call from main loop at ~1 Hz: check WiFi, manage AP
    void checkButton();                // Call every main-loop iteration: 1 Hz sampling mis-measures
                                       // hold times by up to ±1 s (2 s vs 10 s thresholds)
    bool isAPActive() const { return _apActive; }
    void setChangePending(bool pending); // Set/clear the NVS flag
    void activateNow();                  // Immediately enable fallback AP (no timeout)
    void getCachedSSID(char *buf, size_t bufLen) const; // Return cached SSID (no NVS I/O)
    uint32_t getWifiUptimeSeconds() const;

private:
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
    uint32_t _buttonPressStart = 0;      // uptime_ms() when button was first pressed (0 = not pressed)
    bool     _buttonTriggered = false;   // Prevent repeat triggers
    char     _cachedSSID[33] = "";       // Cached SSID to avoid NVS reads on every status poll
};

extern WifiRecovery wifiRecovery;
