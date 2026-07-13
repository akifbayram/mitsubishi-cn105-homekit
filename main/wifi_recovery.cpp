#include "wifi_recovery.h"
#include "improv_serial.h"
#include "espnow_link.h"
#include "dns_server.h"
#include "settings.h"
#include "logging.h"
#include "branding.h"
#include "wifi_manager.h"
#include "esp_utils.h"
#include "status_led.h"

#include <esp_netif.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>   // vTaskDelay/pdMS_TO_TICKS — do not rely on a
#include <freertos/task.h>       // transitive include (absent on esp32c6)

static const char *TAG = "wifi_rec";

WifiRecovery wifiRecovery;

uint32_t WifiRecovery::safeUptimeMs() {
    uint32_t ms = uptime_ms();
    return ms ? ms : 1;  // Avoid 0 sentinel
}

void WifiRecovery::begin(const char *apName, const char *displayName) {
    strncpy(_apName, apName, sizeof(_apName) - 1);
    _apName[sizeof(_apName) - 1] = '\0';
    strncpy(_displayName, displayName, sizeof(_displayName) - 1);
    _displayName[sizeof(_displayName) - 1] = '\0';

#if PIN_BUTTON >= 0
    gpio_set_direction((gpio_num_t)WIFI_RESET_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)WIFI_RESET_BUTTON_PIN, GPIO_PULLUP_ONLY);
#endif

    refreshCachedSSID();

    LOG_INFO("[WiFiRecovery] Initialized (changePending=%s)",
             settings.get().wifiChangePending ? "true" : "false");
}

void WifiRecovery::loop() {
    // Button first, every call — the 2 s / 10 s hold thresholds need finer
    // sampling than the 1 Hz WiFi check below.
    checkButton();

    uint32_t now = uptime_ms();
    if (now - _lastWifiCheck < 1000) return;
    _lastWifiCheck = now;

    bool connected = WifiManager::isConnected();

    // Inside a change window with no reprovision yet, a (re)connect edge is
    // the STA blipping back onto the OLD network (the portal's scan or beacon
    // loss dropped it; auto-reconnect rejoined) — NOT a completed change.
    // Treating it as one cleared the pending flag, cancelled the window
    // deadline and linger-closed the AP mid-provisioning (on-device round 2).
    bool oldNetBlip = _changeWindow && !_reprovisioned;

    // ── WiFi state transitions ──────────────────────────────────────────────
    if (connected && !_wasConnected) {
        // Just connected
        LOG_INFO("[WiFiRecovery] WiFi connected%s",
                 oldNetBlip ? " (old-network blip in change window)" : "");
        _disconnectedSince = 0;
        _wifiConnectedSince = safeUptimeMs();

        if (settings.get().wifiChangePending && !oldNetBlip) {
            setChangePending(false);
            LOG_INFO("[WiFiRecovery] Cleared wifiChangePending");
        }

        if (_apActive && _apShutdownAt == 0 && !oldNetBlip) {
            // Delay AP shutdown so recovery page can confirm connection
            _apShutdownAt = safeUptimeMs() + WIFI_AP_LINGER_MS;
            LOG_INFO("[WiFiRecovery] AP shutdown in %lums (linger for recovery page)",
                     (unsigned long)WIFI_AP_LINGER_MS);
        }
    } else if (!connected && _wasConnected) {
        // Just disconnected
        _disconnectedSince = safeUptimeMs();
        _wifiConnectedSince = 0;
        if (!oldNetBlip) _apShutdownAt = 0;  // Cancel pending AP shutdown (a blip keeps the window deadline)
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

    // ── Reprovision completed (change window) ───────────────────────────────
    // Level, not edge: the 1 Hz poll can miss the deliberate drop entirely
    // when the new join is fast, so don't rely on the transitions above.
    // connected && !isJoinPending() == the STA holds an IP obtained with the
    // credentials the portal applied — collapse the window deadline to the
    // short linger so the phone's recovery page can confirm, then close.
    if (_changeWindow && _reprovisioned && _apActive && connected &&
        !WifiManager::isJoinPending()) {
        if (settings.get().wifiChangePending) {
            setChangePending(false);
            LOG_INFO("[WiFiRecovery] Cleared wifiChangePending (reprovision joined)");
        }
        uint32_t lingerAt = safeUptimeMs() + WIFI_AP_LINGER_MS;
        if (_apShutdownAt == 0 || _apShutdownAt > lingerAt) {
            _apShutdownAt = lingerAt;
            LOG_INFO("[WiFiRecovery] Reprovision joined — AP shutdown in %lums",
                     (unsigned long)WIFI_AP_LINGER_MS);
        }
    }

    // ── Deferred AP shutdown ─────────────────────────────────────────────────
    if (_apShutdownAt > 0 && uptime_ms() >= _apShutdownAt) {
        if (_apActive && connected) {
            _apShutdownAt = 0;
            disableFallbackAP();
        } else if (!_apActive) {
            _apShutdownAt = 0;
        }
        // else: AP up but STA down at the deadline (window expired mid-blip) —
        // keep the deadline armed and close on the next pass once connected,
        // instead of consuming it and leaving the AP up forever.
    }

    // ── DNS captive portal runs in its own task — no processNextRequest() needed
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
    // Improv Serial and the ESP-NOW REPL share the single USB-Serial-JTAG console;
    // starting both deadlocks app_main. Only start Improv if the REPL hasn't taken
    // the console (it's deferred until WiFi is up + AP down, so in this AP path it
    // normally hasn't). Without this guard, the recovery web UI never comes up.
    if (!espnow_console_started()) {
        improv_serial_start(_displayName);
    } else {
        LOG_INFO("[WiFiRecovery] Improv skipped (ESP-NOW console owns serial)");
    }

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
    _changeWindow = false;     // every close path ends the change window
    _reprovisioned = false;
    improv_serial_stop();
}

void WifiRecovery::setChangePending(bool pending) {
    settings.get().wifiChangePending = pending;
    settings.save();
    refreshCachedSSID();
}

void WifiRecovery::activateNow() {
    enableFallbackAP();
}

void WifiRecovery::beginChangeWindow() {
    // One NVS write, not one per dial re-send (~1 Hz until STATE echoes AP-up)
    if (!settings.get().wifiChangePending) setChangePending(true);
    enableFallbackAP();
    // A fresh window starts un-reprovisioned; dial re-sends while the window
    // is already open must not wipe a reprovision that just happened.
    if (!_changeWindow) {
        _changeWindow = true;
        _reprovisioned = false;
    }
    // STA still up = no disconnect will ever tear the AP down; arm the bounded
    // window instead. It collapses to the short linger once a reprovision
    // joins (level check in loop()); un-reprovisioned blips leave it alone.
    if (WifiManager::isConnected())
        _apShutdownAt = safeUptimeMs() + WIFI_SETUP_WINDOW_MS;
}

void WifiRecovery::noteReprovision() {
    _reprovisioned = true;
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
        if (_buttonPressStart != 0 && !_buttonTriggered) {
            uint32_t held = uptime_ms() - _buttonPressStart;
            // Upper bound: a hold that reached 10 s is WiFi-erase territory even
            // if the while-pressed check was missed — never fall into the
            // (destructive when bonded) pairing/unbond path on release.
            if (held >= PAIR_BUTTON_HOLD_MS && held < WIFI_RESET_BUTTON_HOLD_MS) {
#if ESPNOW_REMOTE_ENABLE
                bool otaBusy = false;
#if PIN_LED_DATA >= 0
                otaBusy = (statusLED.getState() == SLED_OTA);
#endif
                if (otaBusy) {
                    LOG_INFO("[WiFiRecovery] Button pairing ignored (OTA in progress)");
                } else if (espnowLink.pairingActive()) {
                    LOG_INFO("[WiFiRecovery] Button: cancelling Link pairing");
                    espnowLink.cancelPairing();
                } else if (espnowLink.isBonded()) {
                    LOG_WARN("[WiFiRecovery] Button hold — forgetting Link remote");
                    espnow_forget_and_restart();
                } else {
                    LOG_INFO("[WiFiRecovery] Button hold — opening Link pairing");
                    espnowLink.startPairing();
                }
#endif
            }
        }
        _buttonPressStart = 0;
        _buttonTriggered = false;
    }
#endif
}
