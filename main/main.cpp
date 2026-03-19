#include <cstdio>
#include <cstring>

#include <nvs_flash.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "logging.h"
#include "settings.h"
#include "board_profile.h"
#include "branding.h"
#include "ble_config.h"
#include "compat_arduino.h"
#include "status_led.h"
#include "cn105_protocol.h"
#include "wifi_manager.h"
#include "wifi_recovery.h"
#include "homekit_setup.h"
#include "homekit_services.h"
#include "web_server.h"

#ifdef BLE_ENABLE
#include "ble_sensor.h"
#endif

static const char *TAG = "main";

// ── Firmware version ────────────────────────────────────────────────────────
#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif

// ── Global instances ────────────────────────────────────────────────────────
CN105Controller cn105;

#if PIN_LED_DATA >= 0
StatusLED statusLED(PIN_LED_DATA, PIN_LED_ENABLE);
#endif

// ── State flags ─────────────────────────────────────────────────────────────
static bool webUIStarted      = false;
static bool homekitStarted    = false;
static bool firmwareValidated = false;
static bool lastAPState       = false;
static uint32_t webUIStartTime = 0;

// ════════════════════════════════════════════════════════════════════════════
// app_main — initialization + main loop
// ════════════════════════════════════════════════════════════════════════════

extern "C" void app_main(void)
{
    // ── 1. NVS flash init ────────────────────────────────────────────────
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // ── 2. Load persistent settings from NVS ─────────────────────────────
    settings.begin();

    // ── 3. Logging init + apply saved log level ──────────────────────────
    logging_init();
    logging_set_level(settings.get().logLevel);

    LOG_INFO("═══════════════════════════════════════");
    LOG_INFO("Mitsubishi CN105 HomeKit Controller");
    LOG_INFO("Board: %s  FW: %s", BOARD_NAME, FW_VERSION);
    LOG_INFO("Log level: %d (0=ERR 1=WARN 2=INFO 3=DBG)", (int)settings.get().logLevel);
    LOG_INFO("═══════════════════════════════════════");

    // ── 4. Status LED begin + boot indicator ─────────────────────────────
#if PIN_LED_DATA >= 0
    statusLED.begin();
    statusLED.setState(SLED_BOOT);
#endif

    // ── 5. Generate AP name from WiFi MAC ────────────────────────────────
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    static char apName[16];
    snprintf(apName, sizeof(apName), BRAND_AP_PREFIX "-%02X%02X", mac[4], mac[5]);

    // Serial number derived from full MAC
    static char serialNumber[18];
    snprintf(serialNumber, sizeof(serialNumber), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Display name: "<brand> XXYY" (unique per device, for mDNS)
    static char displayName[32];
    snprintf(displayName, sizeof(displayName), BRAND_NAME " %02X%02X", mac[4], mac[5]);

    LOG_INFO("AP SSID: %s", apName);
    LOG_INFO("Serial: %s", serialNumber);

    // ── 6. WiFi init ─────────────────────────────────────────────────────
    WifiManager::init(apName, apName, BRAND_AP_PASSWORD);

    // ── 7. Load saved WiFi credentials and connect ───────────────────────
    {
        char ssid[33] = {};
        char pass[65] = {};

#ifdef WIFI_SSID
        // Build-time credentials override NVS
        strncpy(ssid, WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, WIFI_PASSWORD, sizeof(pass) - 1);
        LOG_INFO("Using build-time WiFi credentials (SSID: %s)", ssid);
        WifiManager::connect(ssid, pass);
#else
        if (WifiManager::loadCredentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
            LOG_INFO("Loaded WiFi credentials (SSID: %s)", ssid);
            WifiManager::connect(ssid, pass);
        } else {
            LOG_WARN("No WiFi credentials found — will start recovery AP");
        }
#endif
    }

    // ── 8. Wait for WiFi with timeout ────────────────────────────────────
    LOG_INFO("Waiting for WiFi connection (timeout 15s)...");
    bool wifiOk = WifiManager::waitForConnection(15000);
    if (wifiOk) {
        LOG_INFO("WiFi connected");
#if PIN_LED_DATA >= 0
        statusLED.setState(SLED_WIFI_CONNECTED);
#endif
    } else {
        LOG_WARN("WiFi connection timed out");
#if PIN_LED_DATA >= 0
        statusLED.setState(SLED_WIFI_CONNECTING);
#endif
    }

    // ── 9. HomeKit init (deferred) ─────────────────────────────────────
    // HomeKit is initialized in the main loop after WiFi connects.
    // This keeps port 80 free for the captive portal redirect server
    // during initial WiFi provisioning.

    // ── 10. CN105 UART init ──────────────────────────────────────────────
    cn105.setUpdateInterval(settings.get().pollMs);
    cn105.begin(CN105_UART_NUM, PIN_CN105_RX, PIN_CN105_TX);
    LOG_INFO("CN105 UART started (RX=%d TX=%d baud=%lu)",
             PIN_CN105_RX, PIN_CN105_TX, (unsigned long)CN105_BAUD_RATE);

    // ── 11. WiFi recovery (AP fallback + button handler) ─────────────────
    wifiRecovery.begin(apName);

    // If no WiFi credentials exist, activate recovery AP immediately
    if (!WifiManager::isConnected()) {
        LOG_INFO("Starting recovery AP immediately (no credentials or connection failed)");
        wifiRecovery.activateNow();
    }

    // ── 12. BLE sensor init ──────────────────────────────────────────────
    // BLE is started later, after web UI is up

    // ════════════════════════════════════════════════════════════════════════
    // Main loop
    // ════════════════════════════════════════════════════════════════════════
    LOG_INFO("Entering main loop");

    while (true) {
        // ── CN105 polling ────────────────────────────────────────────────
        cn105.loop();

        // ── Deferred HomeKit init (one-shot after WiFi connects) ─────────
        if (!homekitStarted && WifiManager::isConnected()) {
            // Release port 80 if captive portal redirect server is running
            if (lastAPState) {
                webUI.setAPMode(false);
                lastAPState = false;
            }
            homekit_services_set_controller(&cn105);
            homekitStarted = homekit_init(displayName, BRAND_MANUFACTURER,
                                           BRAND_MODEL, serialNumber, FW_VERSION,
                                           apName);
            if (!homekitStarted) {
                LOG_ERROR("HomeKit init failed, will retry next loop");
            }
        }

        // ── Push state to HomeKit (throttled internally) ─────────────────
        if (homekitStarted) {
            homekit_sync_thermostat(cn105);
            homekit_sync_fan(cn105);
            homekit_sync_switches(cn105);
        }
        // ── WiFi recovery + AP mode tracking ─────────────────────────────
        wifiRecovery.loop();

        // ── Web server deferred init (one-shot after WiFi or AP active) ──
        if (!webUIStarted && (WifiManager::isConnected() || wifiRecovery.isAPActive())) {
            webUI.begin(&cn105);
            webUIStarted = true;
            webUIStartTime = millis();
            LOG_INFO("Web UI started (port 8080)");

#ifdef BLE_ENABLE
            BleSensor::begin();
            LOG_INFO("BLE sensor scanning started");
#endif
        }

        if (webUIStarted) {
            // Track AP mode changes for web UI routing
            // Don't start redirect server on port 80 once HAP owns it
            bool apNow = wifiRecovery.isAPActive();
            if (apNow != lastAPState) {
                if (!homekitStarted || !apNow) {
                    webUI.setAPMode(apNow);
                }
                lastAPState = apNow;
            }

            // WebSocket state push + BLE discovery updates
            webUI.loop();

#ifdef BLE_ENABLE
            BleSensor::loop(cn105);
#endif
        }

        // ── Status LED priority evaluation ───────────────────────────────
#if PIN_LED_DATA >= 0
        if (statusLED.getState() != SLED_OTA) {
            if (!webUIStarted) {
                // Boot phase — show WiFi status
                if (WifiManager::isConnected()) {
                    statusLED.setState(SLED_WIFI_CONNECTED);
                } else {
                    static bool bootBlinked = false;
                    if (!bootBlinked && millis() > 2000) {
                        statusLED.setState(SLED_WIFI_CONNECTING);
                        bootBlinked = true;
                    }
                }
            } else {
                // Normal operation: OTA handled by OTA handler setting SLED_OTA
                const CN105State &st = cn105.getState();
                if (st.hasError) {
                    statusLED.setState(SLED_ERROR_CODE);
                } else if (!cn105.isConnected()) {
                    statusLED.setState(SLED_CN105_DISCONNECTED);
                } else if (!WifiManager::isConnected()) {
                    statusLED.setState(SLED_WIFI_CONNECTING);
                } else {
                    statusLED.setState(SLED_OFF);
                }
            }
        }
        statusLED.loop();
#endif

        // ── OTA rollback validation (one-shot) ──────────────────────────
        // Validate firmware after WiFi + CN105 confirmed working, or 60s timeout
        if (!firmwareValidated && webUIStarted) {
            if (cn105.isConnected() || (millis() - webUIStartTime > 60000)) {
                esp_ota_mark_app_valid_cancel_rollback();
                firmwareValidated = true;
                LOG_INFO("Firmware validated (%s)",
                         cn105.isConnected() ? "WiFi + CN105 OK" : "WiFi OK, CN105 timeout");
            }
        }

        // ── Yield to other FreeRTOS tasks ────────────────────────────────
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
