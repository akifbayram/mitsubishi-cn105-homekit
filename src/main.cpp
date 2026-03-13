#include <Arduino.h>
#include <HomeSpan.h>
#include <WiFi.h>
#include <driver/uart.h>
#include <esp_ota_ops.h>
#include <esp_mac.h>
#include "cn105_protocol.h"
#include "homekit_services.h"
#include "settings.h"
#include "web_server.h"
#include "wifi_recovery.h"
#include "status_led.h"
#include "branding.h"
#include "board_profile.h"

// ── Global instances ─────────────────────────────────────────────────────────
#if USE_HWCDC_DEBUG
HWCDC DebugLog;
#endif
LogLevel currentLogLevel = LOG_LEVEL_INFO;  // Default log level
CN105Controller cn105;
#if PIN_LED_DATA >= 0
StatusLED statusLED(PIN_LED_DATA, PIN_LED_ENABLE);
#endif
static bool webUIStarted = false;
static bool firmwareValidated = false;
static uint32_t webUIStartTime = 0;

// ════════════════════════════════════════════════════════════════════════════
// Arduino Setup
// ════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);

#if USE_HWCDC_DEBUG
    DebugLog.begin();
#endif
#if PIN_LED_DATA >= 0
    statusLED.begin();
    statusLED.setState(SLED_BOOT);
#endif
    LOG_INFO("[MAIN] ═══════════════════════════════════════");
    LOG_INFO("[MAIN] HomeKit controller starting");
    LOG_INFO("[MAIN] Board: %s", BOARD_NAME);
    LOG_INFO("[MAIN] Log level: %d (0=ERR 1=WARN 2=INFO 3=DBG)", currentLogLevel);
    LOG_INFO("[MAIN] ═══════════════════════════════════════");

    // ── Load persistent settings from NVS ─────────────────────────────────
    settings.begin();
    currentLogLevel = settings.get().logLevel;
    cn105.setUpdateInterval(settings.get().pollMs);

    // ── HomeSpan Configuration ──────────────────────────────────────────────
    // Setup code is auto-generated on first boot and stored in NVS.
    // WiFi credentials are entered via captive portal (AP: <prefix>-XXXX).
    // Printed QR label included with each unit for HomeKit pairing.
    //
    // Re-provisioning:
    //   - Serial 'A': launch AP portal manually
    //   - Serial 'X': erase WiFi credentials (AP auto-starts)
    //   - Web UI: change WiFi credentials or reset HomeKit pairing

    homeSpan.setLogLevel(2);
    homeSpan.enableOTA();
    homeSpan.setSketchVersion("0.1.0");
    homeSpan.setHostNameSuffix("");

    // Derive unique device name from WiFi MAC: <prefix>-XXXX
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static char apName[16];
    snprintf(apName, sizeof(apName), BRAND_AP_PREFIX "-%02X%02X", mac[4], mac[5]);

    // Set WiFi/DHCP hostname so router sees "<prefix>-XXXX" instead of "esp32c6-XXXXXX"
    WiFi.setHostname(apName);
    homeSpan.setApSSID(apName);
    homeSpan.setApPassword(BRAND_AP_PASSWORD);
    LOG_INFO("[MAIN] AP SSID: %s", apName);
    homeSpan.enableAutoStartAP();
    // Override HomeSpan's blocking AP page — our recovery system handles it
    homeSpan.setApFunction([]() {
        LOG_INFO("[MAIN] No WiFi credentials — activating recovery AP immediately");
        wifiRecovery.activateNow();
    });
    // QR Setup ID must be unique per device so iOS can distinguish multiple
    // accessories when scanning QR codes (used in mDNS Setup Hash "sh" field)
    static char qrID[5];
    snprintf(qrID, sizeof(qrID), "%02X%02X", mac[4], mac[5]);
    homeSpan.setQRID(qrID);

    // ── Setup code: auto-generate on first boot, persist in NVS ───────────
    if (strlen(settings.get().setupCode) == 0) {
        uint32_t code;
        do {
            code = esp_random() % 100000000;
        } while (code < 10000000);  // Ensure 8 digits
        snprintf(settings.get().setupCode, sizeof(settings.get().setupCode), "%08lu", (unsigned long)code);
        settings.save();
        LOG_INFO("[MAIN] Generated new setup code: %s", settings.get().setupCode);
    } else {
        LOG_INFO("[MAIN] Setup code: %s", settings.get().setupCode);
    }
    homeSpan.setPairingCode(settings.get().setupCode);
    webUI.setSetupCode(settings.get().setupCode);
    webUI.setQRID(qrID);

    // Display name must be unique per device on the network (HomeSpan/mDNS requirement)
    static char displayName[32];
    snprintf(displayName, sizeof(displayName), BRAND_NAME " %02X%02X", mac[4], mac[5]);
    homeSpan.begin(Category::Thermostats, displayName, apName);

    // ── Accessory 1: Bridge / Thermostat ────────────────────────────────────
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Identify();
            new Characteristic::Name("Mini Split");
            new Characteristic::Manufacturer(BRAND_MANUFACTURER);
            new Characteristic::Model(BRAND_MODEL);
            new Characteristic::FirmwareRevision("0.1.0");

        new MitsubishiThermostat(&cn105);
        new MitsubishiFan(&cn105);
        new MitsubishiFanAutoSwitch(&cn105);
        new MitsubishiFanModeSwitch(&cn105);
        new MitsubishiDryModeSwitch(&cn105);

    // ── Initialize CN105 serial communication ───────────────────────────────
    // Pin assignments from board profile (see include/boards/)
    cn105.begin(CN105_UART_NUM, PIN_CN105_RX, PIN_CN105_TX);

    // WiFi recovery: monitors connection, manages AP fallback, handles button
    wifiRecovery.begin(apName);

    // Web UI server starts after WiFi connects or AP fallback activates (deferred to loop)
}

// ════════════════════════════════════════════════════════════════════════════
// Arduino Loop
// ════════════════════════════════════════════════════════════════════════════

void loop() {
    homeSpan.poll();
    cn105.loop();
    wifiRecovery.loop();

    // Start web server once WiFi is connected OR fallback AP is active
    if (!webUIStarted && (WiFi.status() == WL_CONNECTED || wifiRecovery.isAPActive())) {
        webUI.begin(&cn105);
        webUIStarted = true;
        webUIStartTime = millis();
    }
    if (webUIStarted) {
        static bool lastAPActive = false;
        bool apNow = wifiRecovery.isAPActive();
        if (apNow != lastAPActive) {
            webUI.setAPMode(apNow);
            lastAPActive = apNow;
        }
        webUI.loop();
    }

#if PIN_LED_DATA >= 0
    // ── Status LED priority evaluation ──────────────────────────────────
    if (statusLED.getState() != SLED_OTA) {
        if (!webUIStarted) {
            // Boot phase
            if (WiFi.status() == WL_CONNECTED) {
                statusLED.setState(SLED_WIFI_CONNECTED);
            } else {
                static bool bootBlinked = false;
                if (!bootBlinked && millis() > 2000) {
                    statusLED.setState(SLED_WIFI_CONNECTING);
                    bootBlinked = true;
                }
            }
        } else {
            // Normal operation: evaluate error/warning conditions
            const CN105State &st = cn105.getState();
            if (st.hasError) {
                statusLED.setState(SLED_ERROR_CODE);
            } else if (!cn105.isConnected()) {
                statusLED.setState(SLED_CN105_DISCONNECTED);
            } else if (WiFi.status() != WL_CONNECTED) {
                statusLED.setState(SLED_WIFI_CONNECTING);
            } else {
                statusLED.setState(SLED_OFF);
            }
        }
    }
    statusLED.loop();
#endif

    // Validate firmware after WiFi + CN105 confirmed working (or 60s timeout)
    if (!firmwareValidated && webUIStarted) {
        if (cn105.isConnected() || (millis() - webUIStartTime > 60000)) {
            esp_ota_mark_app_valid_cancel_rollback();
            firmwareValidated = true;
            LOG_INFO("[MAIN] Firmware validated (%s)",
                     cn105.isConnected() ? "WiFi + CN105 OK" : "WiFi OK, CN105 timeout");
        }
    }
}
