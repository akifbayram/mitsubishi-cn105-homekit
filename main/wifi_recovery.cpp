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
#include "web_server.h"

#include <esp_netif.h>
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

    refreshCachedSSID();

    // A change left pending across a reboot is still an unconfirmed change:
    // start the failure clock at boot so wifi_err asserts if the (new) creds
    // keep failing to join, instead of silently reading clean until the next set.
    if (settings.get().wifiChangePending) _changeAt = safeUptimeMs();

    LOG_INFO("[WiFiRecovery] Initialized (changePending=%s)",
             settings.get().wifiChangePending ? "true" : "false");
}

void WifiRecovery::loop() {
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

    // ── Credential-change failure latch (SL2 wifi_err) ──────────────────────
    // A change that never joins. _changeAt was stamped by setChangePending
    // (true); if the STA still isn't up after the change-recovery timeout, the
    // new credentials didn't take. Latch it (fires the AP fallback too, on its
    // own path) so the dial can surface the failure. A successful join calls
    // setChangePending(false) on the connect edge above, clearing the latch.
    if (_changeAt != 0 && !connected &&
        uptime_ms() - _changeAt >= WIFI_RECOVERY_TIMEOUT_CHANGE) {
        if (!_changeFailed)
            LOG_WARN("[WiFiRecovery] Credential change failed to join within %lus",
                     (unsigned long)(WIFI_RECOVERY_TIMEOUT_CHANGE / 1000));
        _changeFailed = true;
    }

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
    // improv_serial_start() itself refuses when the ESP-NOW REPL owns the
    // console (they share it; starting both deadlocks app_main).
    improv_serial_start(_displayName);

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
    // Single choke point for every credential change (dial WIFI_SETUP,
    // portal /wifi-setup, Improv). (Re)start the failure clock on a new
    // attempt; clear the latch when the change is confirmed by a join, which
    // is the only caller that passes false (the connect edge in loop()).
    _changeAt = pending ? safeUptimeMs() : 0;
    _changeFailed = false;
    refreshCachedSSID();
}

bool WifiRecovery::wifiChangeFailed() const {
    // Two detectors, one answer: WifiManager's trial latch covers the common
    // runtime failure (new creds rejected at the 30 s deadline, device
    // reverted — the rejoin edge clears _changeAt long before the clock below
    // could fire); the _changeFailed clock covers what the trial can't see, a
    // change left pending across a reboot that never joins (trial state
    // resets at boot).
    //
    // The trial latch is deliberately never cleared — the recovery portal
    // polls /wifi-status to report the verdict, and main.cpp reads it as an
    // edge. But "wifi_err" on the dial is a live fault light, not a history
    // entry: once the reverted network is back the unit is healthy and the
    // dial must stop showing a fault. Gate the latch on still being off the
    // air; if the revert also fails, we stay disconnected and it keeps
    // showing.
    return _changeFailed ||
           (WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_FAILED &&
            !WifiManager::isConnected());
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

uint32_t WifiRecovery::buttonHeldMs() const {
    return _buttonHeldMs;
}

void WifiRecovery::onButton(const ButtonOut& b) {
#if PIN_BUTTON < 0
    (void)b;
#else
    // Zero same-tick on release (not b.heldMs verbatim): heldMs still carries
    // the final hold duration on the BTN_EV_RELEASE tick itself, and the old
    // gpio-polled checkButton() always read 0 the instant the button came up
    // (it zeroed _buttonPressStart before returning). Copying b.heldMs as-is
    // would leave the LED reporting a stale non-zero hold for one extra loop
    // iteration after every release.
    _buttonHeldMs = b.pressed ? b.heldMs : 0;

    if (b.pressed) {
        if (!_buttonTriggered && b.heldMs >= WIFI_RESET_BUTTON_HOLD_MS) {
            _buttonTriggered = true;
            LOG_WARN("[WiFiRecovery] Button held 10s — erasing WiFi credentials");
            WifiManager::eraseCredentials();
            esp_restart();
        }
        return;
    }

    if (b.ev == BTN_EV_RELEASE && !_buttonTriggered) {
        // Upper bound: once the LED enters its red SLED_BTN_WIPE warning tier
        // (>= SLED_BTN_WIPE_WARN_MS, 7 s), releasing must be a no-op so that
        // tier honestly means "release now to abort" — otherwise releasing at,
        // say, 8 s to abort the wipe would instead fire the (destructive when
        // bonded) pairing/unbond action. The 10 s wipe threshold itself is
        // unaffected: it fires from the while-pressed branch above.
        uint32_t held = b.heldMs;
        if (held >= PAIR_BUTTON_HOLD_MS && held < SLED_BTN_WIPE_WARN_MS) {
#if ESPNOW_REMOTE_ENABLE
            bool otaBusy = webota_active();
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

    if (!b.pressed) _buttonTriggered = false;
#endif
}
