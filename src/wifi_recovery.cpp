#include "wifi_recovery.h"
#include "settings.h"
#include "logging.h"
#include "branding.h"
#include <WiFi.h>
#include <HomeSpan.h>
#include <nvs_flash.h>
#include <driver/gpio.h>

WifiRecovery wifiRecovery;

uint32_t WifiRecovery::safeMillis() {
    uint32_t ms = millis();
    return ms ? ms : 1;  // Avoid 0 sentinel
}

void WifiRecovery::begin(const char *apName) {
    strncpy(_apName, apName, sizeof(_apName) - 1);
    _apName[sizeof(_apName) - 1] = '\0';

    // Configure GPIO9 as input with pull-up (NanoC6 BOOT button is active-low)
    pinMode(WIFI_RESET_BUTTON_PIN, INPUT_PULLUP);

    refreshCachedSSID();

    LOG_INFO("[WiFiRecovery] Initialized (changePending=%s)",
             settings.get().wifiChangePending ? "true" : "false");
}

void WifiRecovery::loop() {
    bool connected = (WiFi.status() == WL_CONNECTED);

    // ── WiFi state transitions ──────────────────────────────────────────────
    if (connected && !_wasConnected) {
        // Just connected
        LOG_INFO("[WiFiRecovery] WiFi connected");
        _disconnectedSince = 0;
        _wifiConnectedSince = safeMillis();

        if (settings.get().wifiChangePending) {
            setChangePending(false);
            LOG_INFO("[WiFiRecovery] Cleared wifiChangePending");
        }

        if (_apActive && _apShutdownAt == 0) {
            // Delay AP shutdown so recovery page can confirm connection
            _apShutdownAt = safeMillis() + WIFI_AP_LINGER_MS;
            LOG_INFO("[WiFiRecovery] AP shutdown in %lums (linger for recovery page)",
                     (unsigned long)WIFI_AP_LINGER_MS);
        }
    } else if (!connected && _wasConnected) {
        // Just disconnected
        _disconnectedSince = safeMillis();
        _wifiConnectedSince = 0;
        _apShutdownAt = 0;  // Cancel pending AP shutdown
        LOG_WARN("[WiFiRecovery] WiFi disconnected, starting recovery timer");
    } else if (!connected && _disconnectedSince > 0 && !_apActive) {
        // Still disconnected — check timeout
        uint32_t timeout = settings.get().wifiChangePending
            ? WIFI_RECOVERY_TIMEOUT_CHANGE
            : WIFI_RECOVERY_TIMEOUT_NORMAL;
        if (millis() - _disconnectedSince >= timeout) {
            enableFallbackAP();
        }
    }

    // Handle first boot: if credentials exist but never connected, start timer
    if (!connected && !_wasConnected && _disconnectedSince == 0) {
        _disconnectedSince = safeMillis();
    }

    _wasConnected = connected;

    // ── Deferred AP shutdown ─────────────────────────────────────────────────
    if (_apShutdownAt > 0 && millis() >= _apShutdownAt) {
        _apShutdownAt = 0;
        if (_apActive && connected) {
            disableFallbackAP();
        }
    }

    // ── Captive portal DNS ──────────────────────────────────────────────────
    if (_apActive) {
        _dnsServer.processNextRequest();
    }

    // ── Button check ────────────────────────────────────────────────────────
    checkButton();
}

void WifiRecovery::enableFallbackAP() {
    if (_apActive) return;

    LOG_WARN("[WiFiRecovery] Enabling fallback AP: %s", _apName);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(_apName, BRAND_AP_PASSWORD);

    // Captive portal: resolve all DNS queries to AP IP
    _dnsServer.start(53, "*", WiFi.softAPIP());

    _apActive = true;
    LOG_INFO("[WiFiRecovery] AP active at %s (captive portal DNS started)",
             WiFi.softAPIP().toString().c_str());
}

void WifiRecovery::disableFallbackAP() {
    if (!_apActive) return;

    LOG_INFO("[WiFiRecovery] Disabling fallback AP (WiFi connected)");
    _dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    _apActive = false;
}

void WifiRecovery::setChangePending(bool pending) {
    settings.get().wifiChangePending = pending;
    settings.save();
    refreshCachedSSID();
}

void WifiRecovery::activateNow() {
    enableFallbackAP();
}

void WifiRecovery::refreshCachedSSID() {
    // Read SSID from HomeSpan's NVS namespace ("WIFI" / "WIFIDATA" blob)
    _cachedSSID[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("WIFI", NVS_READONLY, &handle) != ESP_OK) return;

    struct { char ssid[33]; char pwd[65]; } data;
    memset(&data, 0, sizeof(data));
    size_t len = sizeof(data);
    esp_err_t err = nvs_get_blob(handle, "WIFIDATA", &data, &len);
    nvs_close(handle);

    if (err == ESP_OK && strlen(data.ssid) > 0) {
        strncpy(_cachedSSID, data.ssid, sizeof(_cachedSSID) - 1);
        _cachedSSID[sizeof(_cachedSSID) - 1] = '\0';
    }
}

void WifiRecovery::getCachedSSID(char *buf, size_t bufLen) const {
    strncpy(buf, _cachedSSID, bufLen - 1);
    buf[bufLen - 1] = '\0';
}

uint32_t WifiRecovery::getWifiUptimeSeconds() const {
    if (_wifiConnectedSince == 0) return 0;
    return (safeMillis() - _wifiConnectedSince) / 1000;
}

void WifiRecovery::checkButton() {
    bool pressed = (digitalRead(WIFI_RESET_BUTTON_PIN) == LOW); // Active-low

    if (pressed && _buttonPressStart == 0) {
        _buttonPressStart = safeMillis();
        _buttonTriggered = false;
    } else if (pressed && !_buttonTriggered) {
        if (millis() - _buttonPressStart >= WIFI_RESET_BUTTON_HOLD_MS) {
            _buttonTriggered = true;
            LOG_WARN("[WiFiRecovery] Button held 10s — erasing WiFi credentials");
            homeSpan.processSerialCommand("X"); // Erases WiFi NVS and reboots
        }
    } else if (!pressed) {
        _buttonPressStart = 0;
        _buttonTriggered = false;
    }
}
