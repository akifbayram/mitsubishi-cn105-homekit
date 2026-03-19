#include "wifi_recovery.h"
#include "dns_server.h"
#include "settings.h"
#include "logging.h"
#include "branding.h"
#include "wifi_manager.h"
#include "esp_utils.h"

#include <esp_netif.h>
#include <driver/gpio.h>
#include <esp_system.h>

static const char *TAG = "wifi_rec";

WifiRecovery wifiRecovery;

uint32_t WifiRecovery::safeUptimeMs() {
    uint32_t ms = uptime_ms();
    return ms ? ms : 1;  // Avoid 0 sentinel
}

void WifiRecovery::begin(const char *apName) {
    strncpy(_apName, apName, sizeof(_apName) - 1);
    _apName[sizeof(_apName) - 1] = '\0';

#if PIN_BUTTON >= 0
    gpio_set_direction((gpio_num_t)WIFI_RESET_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)WIFI_RESET_BUTTON_PIN, GPIO_PULLUP_ONLY);
#endif

    refreshCachedSSID();

    LOG_INFO("[WiFiRecovery] Initialized (changePending=%s)",
             settings.get().wifiChangePending ? "true" : "false");
}

void WifiRecovery::loop() {
    bool connected = WifiManager::isConnected();

    // ── WiFi state transitions ──────────────────────────────────────────────
    if (connected && !_wasConnected) {
        // Just connected
        LOG_INFO("[WiFiRecovery] WiFi connected");
        _disconnectedSince = 0;
        _wifiConnectedSince = safeUptimeMs();

        if (settings.get().wifiChangePending) {
            setChangePending(false);
            LOG_INFO("[WiFiRecovery] Cleared wifiChangePending");
        }

        if (_apActive && _apShutdownAt == 0) {
            // Delay AP shutdown so recovery page can confirm connection
            _apShutdownAt = safeUptimeMs() + WIFI_AP_LINGER_MS;
            LOG_INFO("[WiFiRecovery] AP shutdown in %lums (linger for recovery page)",
                     (unsigned long)WIFI_AP_LINGER_MS);
        }
    } else if (!connected && _wasConnected) {
        // Just disconnected
        _disconnectedSince = safeUptimeMs();
        _wifiConnectedSince = 0;
        _apShutdownAt = 0;  // Cancel pending AP shutdown
        LOG_WARN("[WiFiRecovery] WiFi disconnected, starting recovery timer");
    } else if (!connected && _disconnectedSince > 0 && !_apActive) {
        // Still disconnected — check timeout
        uint32_t timeout = settings.get().wifiChangePending
            ? WIFI_RECOVERY_TIMEOUT_CHANGE
            : WIFI_RECOVERY_TIMEOUT_NORMAL;
        if (uptime_ms() - _disconnectedSince >= timeout) {
            enableFallbackAP();
        }
    }

    // Handle first boot: if credentials exist but never connected, start timer
    if (!connected && !_wasConnected && _disconnectedSince == 0) {
        _disconnectedSince = safeUptimeMs();
    }

    _wasConnected = connected;

    // ── Deferred AP shutdown ─────────────────────────────────────────────────
    if (_apShutdownAt > 0 && uptime_ms() >= _apShutdownAt) {
        _apShutdownAt = 0;
        if (_apActive && connected) {
            disableFallbackAP();
        }
    }

    // ── DNS captive portal runs in its own task — no processNextRequest() needed

    // ── Button check ────────────────────────────────────────────────────────
    checkButton();
}

void WifiRecovery::enableFallbackAP() {
    if (_apActive) return;

    LOG_WARN("[WiFiRecovery] Enabling fallback AP: %s", _apName);

    WifiManager::enableAP(_apName, BRAND_AP_PASSWORD);

    // Get AP IP address for captive portal DNS redirect
    esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ipInfo = {};
    uint32_t apIP = 0;
    if (apNetif && esp_netif_get_ip_info(apNetif, &ipInfo) == ESP_OK) {
        apIP = ipInfo.ip.addr;  // already in network byte order
    }

    // Start captive portal DNS
    dns_captive_start(apIP);

    _apActive = true;

    char ipStr[16];
    esp_ip4addr_ntoa(&ipInfo.ip, ipStr, sizeof(ipStr));
    LOG_INFO("[WiFiRecovery] AP active at %s (captive portal DNS started)", ipStr);
}

void WifiRecovery::disableFallbackAP() {
    if (!_apActive) return;

    LOG_INFO("[WiFiRecovery] Disabling fallback AP (WiFi connected)");
    dns_captive_stop();
    WifiManager::disableAP();
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
    _cachedSSID[0] = '\0';
    char ssid[33] = {};
    char pass[65] = {};
    if (WifiManager::loadCredentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strncpy(_cachedSSID, ssid, sizeof(_cachedSSID) - 1);
        _cachedSSID[sizeof(_cachedSSID) - 1] = '\0';
    }
}

void WifiRecovery::getCachedSSID(char *buf, size_t bufLen) const {
    strncpy(buf, _cachedSSID, bufLen - 1);
    buf[bufLen - 1] = '\0';
}

uint32_t WifiRecovery::getWifiUptimeSeconds() const {
    if (_wifiConnectedSince == 0) return 0;
    return (safeUptimeMs() - _wifiConnectedSince) / 1000;
}

void WifiRecovery::checkButton() {
#if PIN_BUTTON < 0
    return;
#else
    bool pressed = (gpio_get_level((gpio_num_t)WIFI_RESET_BUTTON_PIN) == (BUTTON_ACTIVE_LOW ? 0 : 1));

    if (pressed && _buttonPressStart == 0) {
        _buttonPressStart = safeUptimeMs();
        _buttonTriggered = false;
    } else if (pressed && !_buttonTriggered) {
        if (uptime_ms() - _buttonPressStart >= WIFI_RESET_BUTTON_HOLD_MS) {
            _buttonTriggered = true;
            LOG_WARN("[WiFiRecovery] Button held 10s — erasing WiFi credentials");
            WifiManager::eraseCredentials();
            esp_restart();
        }
    } else if (!pressed) {
        _buttonPressStart = 0;
        _buttonTriggered = false;
    }
#endif
}
