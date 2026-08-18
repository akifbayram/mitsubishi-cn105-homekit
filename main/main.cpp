#include <cstdio>
#include <cstring>

#include <nvs_flash.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_console.h>
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include <driver/usb_serial_jtag.h>
#endif
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "logging.h"
#include "event_log.h"
#include "time_sync.h"
#include "settings.h"
#include "board_profile.h"
#include "branding.h"
#include "ble_config.h"
#include "esp_utils.h"
#include "status_led.h"
#include "cn105_protocol.h"
#include "espnow_link.h"
#include "wifi_manager.h"
#include "wifi_recovery.h"
#include "button_input.h"
#include "homekit_setup.h"
#include "homekit_services.h"
#include "web_server.h"
#include "link_sensor.h"
#include "room_avg.h"

#ifdef BLE_ENABLE
#include "ble_sensor.h"
#include "homekit_sensor_accessory.h"
#endif
#include "ble_pair.h"

static const char *TAG = "main";

// ── Firmware version ────────────────────────────────────────────────────────
#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

// ── Global instances ────────────────────────────────────────────────────────
CN105Controller cn105;

#if PIN_LED_DATA >= 0
StatusLED statusLED(PIN_LED_DATA, PIN_LED_ENABLE, PIN_BLUE_LED);
static_assert(SLED_BTN_PAIR_TIER_MS == PAIR_BUTTON_HOLD_MS,
              "LED pair tier must match the button's pairing hold threshold");
#endif

// ── Crash loop detection (RTC NOINIT survives software resets) ───────────────
RTC_NOINIT_ATTR static uint32_t s_crashMagic;
RTC_NOINIT_ATTR static uint32_t s_crashCount;
static constexpr uint32_t CRASH_MAGIC     = 0xDEAD0505;
static constexpr uint32_t CRASH_THRESHOLD = 5;
static bool safeMode = false;

uint32_t appCrashCount(void) { return s_crashCount; }

// ── State flags ─────────────────────────────────────────────────────────────
// (HomeKit started-ness lives in homekit_is_started() — no parallel flag here)
static bool webUIStarted      = false;
static bool espnowConsoleInit = false;   // ESP-NOW REPL one-shot attempted
static bool firmwareValidated = false;
static bool lastAPState       = false;
static uint32_t webUIStartTime = 0;
#if PIN_LED_DATA >= 0
static bool lastWifiState     = true;   // force initial setWifi() call
static const char *lastPairResult = nullptr;   // interned literal from EspnowLink
static WifiManager::WifiTrialState lastTrialState = WifiManager::WIFI_TRIAL_IDLE;
#endif

// ── Diagnostic console commands ─────────────────────────────────────────────
// `wdt-test`: deliberately trip the task watchdog to exercise the whole
// crash → panic → reset-reason → event-log CRASH path end-to-end. Without a
// way to inject this fault, that recovery machinery is only ever tested by
// real bugs. Runs on the console task: subscribe it to the WDT, never feed.
static int cmdWdtTest(int argc, char **argv) {
    LOG_WARN("wdt-test: hanging this task — expect a task-WDT panic + reset in ~10s");
    // CONFIG_ESP_TASK_WDT_PANIC may be off; make the test deterministic.
    esp_task_wdt_config_t cfg = { .timeout_ms = 10000, .idle_core_mask = 1, .trigger_panic = true };
    esp_task_wdt_reconfigure(&cfg);
    esp_task_wdt_add(NULL);
    for (;;) vTaskDelay(pdMS_TO_TICKS(100));   // subscribed, never fed
    return 0;
}

// True when a USB-Serial-JTAG host is actively on the bus (SOF activity per
// the driver's connection monitor). Headless units — powered from the heat
// pump's CN105 connector, no computer attached — return false, and the
// console REPL must not start there (see the guard at the call site).
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static bool console_host_present(void) { return usb_serial_jtag_is_connected(); }
#else
static bool console_host_present(void) { return true; }  // UART console: no host concept
#endif

static void registerDiagConsole(void) {
    const esp_console_cmd_t cmd = {
        .command = "wdt-test",
        .help = "Trip the task watchdog (panic + reset) to test crash handling",
        .hint = nullptr,
        .func = &cmdWdtTest,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    // Tolerate a missing REPL (ESP-NOW compiled out → no console was started)
    if (esp_console_cmd_register(&cmd) != ESP_OK)
        LOG_DEBUG("wdt-test console cmd not registered (no REPL)");
}

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

    // ── 2. Crash loop detection ────────────────────────────────────────
    esp_reset_reason_t resetReason = esp_reset_reason();
    bool wasCrash = (resetReason == ESP_RST_PANIC   || resetReason == ESP_RST_TASK_WDT ||
                     resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_WDT);
    {
        if (s_crashMagic != CRASH_MAGIC) {
            s_crashMagic = CRASH_MAGIC;
            s_crashCount = 0;
        }
        if (wasCrash) {
            s_crashCount++;
        } else {
            s_crashCount = 0;
        }
        safeMode = (s_crashCount >= CRASH_THRESHOLD);
    }

    // ── 3. Load persistent settings from NVS ─────────────────────────────
    settings.begin();

    // ── 4. Logging init + apply saved log level ──────────────────────────
    logging_init();
    logging_set_level(settings.get().logLevel);

    // ── 5. Device event log (persistent boot/crash/fault history) ────────
    eventlog_init(resetReason, wasCrash, safeMode);

    LOG_INFO("═══════════════════════════════════════");
    LOG_INFO("Mitsubishi CN105 HomeKit Controller");
    LOG_INFO("Board: %s  FW: %s", BOARD_NAME, FW_VERSION);
    LOG_INFO("Reset: %s  Crashes: %lu", reset_reason_str(resetReason), (unsigned long)s_crashCount);
    if (safeMode) LOG_WARN(">>> SAFE MODE — %lu consecutive crashes, skipping CN105/HomeKit/BLE <<<", (unsigned long)s_crashCount);
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
    // joinStarted is connect()'s own answer, NOT WifiManager::hasCredentials():
    // that reads NVS, which is empty during a build-time-credentials trial
    // join (connect() persists nothing until GOT_IP), and step 8 gates on
    // "was a join started", not "does NVS hold credentials".
    bool joinStarted = false;
    {
        char ssid[33] = {};
        char pass[65] = {};

#ifdef WIFI_SSID
        // Build-time credentials override NVS
        strncpy(ssid, WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, WIFI_PASSWORD, sizeof(pass) - 1);
        LOG_INFO("Using build-time WiFi credentials (SSID: %s)", ssid);
        joinStarted = WifiManager::connect(ssid, pass);
#else
        if (WifiManager::loadCredentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
            LOG_INFO("Loaded WiFi credentials (SSID: %s)", ssid);
            joinStarted = WifiManager::connect(ssid, pass);
        } else {
            LOG_WARN("No WiFi credentials found — will start recovery AP");
        }
#endif
    }

    // ── 8. Wait for WiFi with timeout ────────────────────────────────────
    // Only when a join is actually in flight. With nothing to wait for, this
    // wait used to delay the recovery AP — and the Improv listener inside it —
    // until t=15s on a fresh flash, past the web flasher's 10s handshake
    // budget.
    if (joinStarted) {
        LOG_INFO("Waiting for WiFi connection (timeout 15s)...");
        if (WifiManager::waitForConnection(15000)) {
            LOG_INFO("WiFi connected");
        } else {
            LOG_WARN("WiFi connection timed out");
        }
    }

    // ── 9. HomeKit init (deferred) ─────────────────────────────────────
    // HomeKit is initialized in the main loop after WiFi connects.
    // This keeps port 80 free for the captive portal redirect server
    // during initial WiFi provisioning.

    // ── 10. CN105 UART init (skipped in safe mode) ─────────────────────
    if (!safeMode) {
        cn105.setUpdateInterval(settings.get().pollMs);
        cn105.begin(CN105_UART_NUM, PIN_CN105_RX, PIN_CN105_TX);
        LOG_INFO("CN105 UART started (RX=%d TX=%d baud=%lu)",
                 PIN_CN105_RX, PIN_CN105_TX, (unsigned long)CN105_BAUD_RATE);
    }

#if PIN_BUTTON >= 0
    gpio_set_direction((gpio_num_t)WIFI_RESET_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)WIFI_RESET_BUTTON_PIN, GPIO_PULLUP_ONLY);
#endif

    // ── 11. WiFi recovery (AP fallback + button handler) ─────────────────
    wifiRecovery.begin(apName, displayName);

    // In safe mode or if no WiFi credentials, activate recovery AP immediately
    if (safeMode || !WifiManager::isConnected()) {
        LOG_INFO("Starting recovery AP immediately (%s)",
                 safeMode ? "safe mode" : "no credentials or connection failed");
        wifiRecovery.activateNow();
    }

    // ── 12. Start CN105 dedicated task (skipped in safe mode) ────────────
    if (!safeMode) {
        cn105.startTask();
    }

    // ── 13. ESP-NOW remote (Dial) — safe no-op when unbonded ───────────────────
    espnowLink.begin(&cn105);
    // NOTE: the ESP-NOW serial REPL is NOT registered here. It and Improv Serial
    // both read the single USB-Serial-JTAG console; starting both deadlocks
    // app_main in the recovery path. The REPL is deferred to a main-loop one-shot
    // gated on (connected && !apActive); Improv only starts when the REPL hasn't
    // (see enableFallbackAP). This guarantees a single console owner.

    // ── 14. BLE sensor init ──────────────────────────────────────────────
    // BLE is started later, after web UI is up (skipped in safe mode)

    // ════════════════════════════════════════════════════════════════════════
    // Main loop — tiered polling
    // CN105 UART runs in its own task; remaining subsystems polled at
    // appropriate rates to reduce unnecessary work.
    // ════════════════════════════════════════════════════════════════════════
    LOG_INFO("Entering main loop");
    esp_task_wdt_add(NULL);

    uint32_t lastWebLoop   = 0;
    uint32_t lastHeapLog   = 0;
    uint32_t lastAliveLog  = 0;
#ifdef BLE_ENABLE
    uint32_t lastBleLoop   = 0;
#endif
    uint32_t lastLinkSensorLoop = 0;

    while (true) {
        esp_task_wdt_reset();
        uint32_t now = uptime_ms();

        // ── Deferred HomeKit init (one-shot after WiFi connects, skipped in safe mode)
        // Retries are backed off to every 10s — a persistent failure (e.g.
        // hap_start() refusing) would otherwise retry every loop iteration
        // and flood the log.
        static uint32_t nextHomekitAttempt = 0;
        if (!safeMode && !homekit_is_started() && WifiManager::isConnected() &&
            now >= nextHomekitAttempt) {
            nextHomekitAttempt = now + 10000;
            // WiFi is up: drop the captive portal handler now that we have a real
            // network. HAP binds its own port (8080), so there's no contention
            // with the web UI on port 80.
            if (lastAPState) {
                webUI.setAPMode(false);
                lastAPState = false;
            }
            homekit_services_set_controller(&cn105);
            if (!homekit_init(displayName, BRAND_MANUFACTURER, BRAND_MODEL,
                              serialNumber, FW_VERSION, apName)) {
                LOG_ERROR("HomeKit init failed, will retry in 10s");
            }
        }

        // ── Deferred ESP-NOW serial REPL (one-shot) ──────────────────────
        // Only take the USB-Serial-JTAG console once WiFi is up AND the fallback
        // AP is down — i.e. when Improv Serial is not running. This keeps a single
        // console owner and avoids the recovery-path deadlock. Consequence: Improv
        // is first-provisioning-only; later re-provisioning is via the AP web UI.
        // Headless guard: with no USB host ever attached, esp_console REPL
        // init can block its caller indefinitely inside console I/O
        // (linenoiseProbe runs on the CALLING task by design — see the
        // comment in esp_console_common.c — and the no-host paths carry
        // IDF-14303 TODOs). Field units run headless off the heat pump
        // connector; starting the REPL there blocked main at ~T+5 (normal)
        // / ~T+10 (safe mode) and the 10 s task-WDT panicked every boot —
        // the 2026-08-05/06 customer crash loop, A/B-proven on the bench
        // (identical build minus this call: 20 s crash metronome vs stable).
        // Gate on live host presence — the check re-arms each pass, so
        // plugging USB in later still brings the REPL up — and run the init
        // on a disposable helper task so main is never exposed even if the
        // connection state is stale.
        if (!espnowConsoleInit &&
            WifiManager::isConnected() && !wifiRecovery.isAPActive() &&
            console_host_present()) {
            // Attempt once (no-op stub when ESP-NOW off); stay re-armed if
            // the helper task could not even be created (transient low heap),
            // so a later pass retries instead of losing the console for good.
            espnowConsoleInit = xTaskCreate([](void *) {
                espnow_register_console();
                registerDiagConsole();  // piggybacks on the REPL the line above started
                vTaskDelete(nullptr);
            }, "repl_init", 4096, nullptr, 2, nullptr) == pdPASS;
        }

        // ── Push state to HomeKit (throttled internally) ─────────────────
        if (homekit_is_started()) {
            homekit_sync_thermostat(cn105);
            homekit_sync_fan(cn105);
            homekit_sync_switches(cn105);
#ifdef BLE_ENABLE
            homekit_sensor_loop();
#endif
        }

        // ── ESP-NOW remote — every iter (~10 ms) ────────────────────────
        espnowLink.loop();

        // ── Button — every iter (~10 ms). One debounced source, two
        // consumers: wifiRecovery owns the hold bands, BlePair owns clicks.
#if PIN_BUTTON >= 0
        {
            static ButtonInput button;
            bool rawPressed = (gpio_get_level((gpio_num_t)WIFI_RESET_BUTTON_PIN)
                               == (BUTTON_ACTIVE_LOW ? 0 : 1));
            ButtonOut b = button.update(rawPressed, now);
            wifiRecovery.onButton(b);
            if (webUIStarted && !safeMode && b.ev == BTN_EV_CLICK && b.clicks == 3) BlePair::onTripleClick();
        }
#endif

        // ── WiFi recovery — every iter (~10 ms); WiFi checks are
        // rate-limited to 1 Hz inside loop()
        wifiRecovery.loop();
        WifiManager::loop();   // trial-connect commit/revert (must run on main task)

        // ── Event log edges + SNTP one-shot — 1 Hz ───────────────────────
        // All app-level event detection lives here (single place, no new
        // coupling inside the protocol/wifi modules). 1 Hz is plenty for
        // these transitions and keeps getState()'s lock + struct copy off
        // the 10 ms path.
        static uint32_t lastEventScan = 0;
        if (now - lastEventScan >= 1000) {
            lastEventScan = now;

            static bool sntpStarted = false;
            if (!sntpStarted && WifiManager::isConnected()) {
                sntpStarted = true;
                time_sync_start();
            }

            // Heat pump link lost/restored (only after the first successful
            // connect — the initial handshake window isn't an outage).
            static bool everConnected = false;
            static bool lastCn105     = false;
            bool cn105Now = cn105.isConnected();
            if (cn105Now != lastCn105) {
                if (cn105Now) {
                    if (everConnected) eventlog_append(EV_CN105_RESTORED);
                    everConnected = true;
                } else if (everConnected) {
                    eventlog_append(EV_CN105_LOST);
                }
                lastCn105 = cn105Now;
            }

            // Heat pump fault code appearing
            static bool lastHasError = false;
            const CN105State evSt = cn105.getState();
            if (evSt.hasError != lastHasError) {
                if (evSt.hasError) eventlog_append(EV_CN105_ERROR, evSt.errorCode);
                lastHasError = evSt.hasError;
            }

            // WiFi recovery AP raised
            static bool lastRecoveryAP = false;
            bool apActive = wifiRecovery.isAPActive();
            if (apActive && !lastRecoveryAP) eventlog_append(EV_RECOVERY_AP);
            lastRecoveryAP = apActive;

            // Free-heap low-water warning (10 KB buckets): WARN the moment
            // the session floor drops into a new bucket, so a leak announces
            // itself in the live log stream instead of hiding in the 60s
            // INFO cadence. The floor is the SDK's own lifetime minimum (same
            // source as the 60s heap INFO), so transient dips between checks
            // count too. First check sets the boot baseline silently; drops
            // that follow are wanted — they make boot-to-boot comparisons
            // meaningful.
            static uint32_t lowestHeapBucket = UINT32_MAX;
            uint32_t heapLow = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
            uint32_t heapBucket = heapLow / 10240;
            if (heapBucket < lowestHeapBucket) {
                if (lowestHeapBucket != UINT32_MAX) {
                    LOG_WARN("Free heap new low: %lu bytes (largest block %lu)",
                             (unsigned long)heapLow,
                             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
                }
                lowestHeapBucket = heapBucket;
            }
        }

        // ── Main-loop alive — 15s ────────────────────────────────────
        if (now - lastAliveLog >= 15000) {
            lastAliveLog = now;
            LOG_DEBUG("main loop alive (uptime=%lus)", (unsigned long)(now / 1000));
        }

        // ── Heap health — 60s ─────────────────────────────────────────
        if (now - lastHeapLog >= 60000) {
            lastHeapLog = now;
            LOG_INFO("Heap: free=%lu min=%lu blk=%lu",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        }

        // ── Web server deferred init (one-shot after WiFi or AP active) ──
        if (!webUIStarted && (WifiManager::isConnected() || wifiRecovery.isAPActive())) {
            webUI.begin(&cn105);
            webUIStarted = true;
            webUIStartTime = uptime_ms();
            LOG_INFO("Web UI started (port 80)");

#ifdef BLE_ENABLE
            if (!safeMode) BleSensor::begin();
            if (!safeMode) BlePair::begin();
#endif
        }

        // ── WebSocket state push + AP tracking — 10 Hz ──────────────────
        if (webUIStarted && now - lastWebLoop >= 100) {
            bool apNow = wifiRecovery.isAPActive();
            if (apNow != lastAPState) {
                // Captive portal handler lives on the port-80 web server and is
                // subnet-gated, so it can safely coexist with HAP (port 8080).
                webUI.setAPMode(apNow);
                lastAPState = apNow;
            }

            webUI.loop();
            lastWebLoop = now;
        }

        // ── BLE keepalive — 1 Hz ────────────────────────────────────────
#ifdef BLE_ENABLE
        if (webUIStarted && now - lastBleLoop >= 1000) {
            BleSensor::loop(cn105);
            BlePair::loop();
            lastBleLoop = now;
        }
#endif

        // ── Link-sensor keepalive — 1 Hz (not BLE-gated: a build without BLE
        //    still supports a dial-sourced room temperature) ────────────────
        if (webUIStarted && now - lastLinkSensorLoop >= 1000) {
            LinkSensor::loop(cn105);
            // Average-mode blend, after both source loops so it sees this
            // tick's readings.
            RoomAvg::loop(cn105);
            lastLinkSensorLoop = now;
        }

        // ── Status LED priority evaluation ───────────────────────────────
#if PIN_LED_DATA >= 0
        // Blue mono LED (NanoC6) tracks WiFi independently; green success
        // flash when WiFi comes up while the setup portal is open.
        {
            bool wifiNow = WifiManager::isConnected();
            if (wifiNow != lastWifiState) {
                statusLED.setWifi(wifiNow);
                if (wifiNow && wifiRecovery.isAPActive()) {
                    statusLED.requestHold(SLED_RESULT_OK, 3000);
                }
                lastWifiState = wifiNow;
            }
        }

        // WiFi credential-trial verdict (edge): submitted creds accepted or
        // rejected → transient result hold.
        WifiManager::WifiTrialState tr = WifiManager::getTrialState();
        {
            if (tr != lastTrialState) {
                if (tr == WifiManager::WIFI_TRIAL_SUCCESS) {
                    statusLED.requestHold(SLED_RESULT_OK, 3000);
                } else if (tr == WifiManager::WIFI_TRIAL_FAILED) {
                    statusLED.requestHold(SLED_RESULT_FAIL, 3000);
                }
                lastTrialState = tr;
            }
        }

        // Link pairing verdict (edge) — hold the LED on the result.
        {
            const char *pr = espnowLink.pairResult();
            if (pr != lastPairResult) {   // interned — pointer compare works
                lastPairResult = pr;
                switch (espnowLink.pairOutcome()) {
                    case ESPNOW_PAIR_OK:   statusLED.requestHold(SLED_RESULT_OK, 5000);  break;
                    case ESPNOW_PAIR_FAIL: statusLED.requestHold(SLED_RESULT_FAIL, 3000); break;
                    default: break;
                }
            }
        }

        // Gather inputs and let the pure policy pick the state (priority
        // order lives in sled_policy.h; requestHold overrides in setState).
        {
            SledInputs li = {};
            li.buttonHeldMs    = wifiRecovery.buttonHeldMs();
            li.otaActive       = webota_active();
#if ESPNOW_REMOTE_ENABLE
            li.pairActionAllowed = !li.otaActive;   // mirrors onButton()'s OTA guard
#endif
            li.pairingActive   = espnowLink.pairingActive();
#ifdef BLE_ENABLE
            li.blePairListening  = BlePair::isListening();
#endif
            li.wifiTrialActive = (tr == WifiManager::WIFI_TRIAL_TESTING);   // real credential trials only
            li.safeMode        = safeMode;
            li.portalActive    = wifiRecovery.isAPActive();
            li.webUIStarted    = webUIStarted;
            li.hpError         = cn105.hasError();   // lock-free; getState() copies 44 B
            li.cn105Connected  = cn105.isConnected();
            li.wifiConnected   = lastWifiState;
            li.wifiOnRgb       = (WIFI_ON_RGB != 0);
            statusLED.setState(sled_evaluate(li));
        }
        statusLED.loop();
#endif

        // ── OTA rollback validation (one-shot, skipped in safe mode) ────
        // In safe mode, don't validate — let bootloader rollback to previous firmware.
        // Validate firmware after WiFi + CN105 confirmed working, or 60s timeout.
        if (!safeMode && !firmwareValidated && webUIStarted) {
            if (cn105.isConnected() || (uptime_ms() - webUIStartTime > 60000)) {
                esp_ota_mark_app_valid_cancel_rollback();
                firmwareValidated = true;
                s_crashCount = 0;  // Clear crash counter on successful validation
                LOG_INFO("Firmware validated (%s)",
                         cn105.isConnected() ? "WiFi + CN105 OK" : "WiFi OK, CN105 timeout");
            }
        }

        // ── Yield to other FreeRTOS tasks ────────────────────────────────
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
