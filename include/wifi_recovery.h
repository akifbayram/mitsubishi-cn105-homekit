#pragma once
#include <Arduino.h>
#include <DNSServer.h>
#include "board_profile.h"

// Timeout before enabling AP fallback
constexpr uint32_t WIFI_RECOVERY_TIMEOUT_CHANGE = 120000;  // 2 min (after credential change)
constexpr uint32_t WIFI_RECOVERY_TIMEOUT_NORMAL = 300000;  // 5 min (after normal disconnect)

// Delay before disabling AP after WiFi reconnects (lets recovery page confirm)
constexpr uint32_t WIFI_AP_LINGER_MS = 6000;  // 6 seconds (recovery page polls every 3s)

// Button long-press duration for WiFi reset
constexpr uint32_t WIFI_RESET_BUTTON_HOLD_MS = 10000;  // 10 seconds
constexpr int8_t   WIFI_RESET_BUTTON_PIN = PIN_BUTTON;  // From board profile (-1 = no button)

class WifiRecovery {
public:
    void begin(const char *apName);    // Initialize: store AP name, configure GPIO9
    void loop();                       // Call from main loop: check WiFi, manage AP, handle button
    bool isAPActive() const { return _apActive; }
    void setChangePending(bool pending); // Set/clear the NVS flag
    void activateNow();                  // Immediately enable fallback AP (no timeout)
    void getCachedSSID(char *buf, size_t bufLen) const; // Return cached SSID (no NVS I/O)
    uint32_t getWifiUptimeSeconds() const;

private:
    void enableFallbackAP();
    void disableFallbackAP();
    void checkButton();
    void refreshCachedSSID();          // Re-read SSID from HomeSpan NVS into _cachedSSID
    static uint32_t safeMillis();      // millis() that never returns 0 (sentinel avoidance)

    DNSServer _dnsServer;
    char     _apName[16] = "";
    bool     _apActive = false;
    bool     _wasConnected = false;      // Track previous WiFi state
    uint32_t _disconnectedSince = 0;     // millis() when WiFi was lost (0 = connected)
    uint32_t _wifiConnectedSince = 0;   // millis() when WiFi connected (0 = not connected)
    uint32_t _apShutdownAt = 0;          // millis() when AP should be disabled (0 = no pending shutdown)
    uint32_t _buttonPressStart = 0;      // millis() when button was first pressed (0 = not pressed)
    bool     _buttonTriggered = false;   // Prevent repeat triggers
    char     _cachedSSID[33] = "";       // Cached SSID to avoid NVS reads on every status poll
};

extern WifiRecovery wifiRecovery;
