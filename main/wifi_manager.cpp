#include "wifi_manager.h"
#include "logging.h"
#include "esp_utils.h"

#include <cstring>
#include <algorithm>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

static const char *TAG = "wifi";

// ── NVS keys ────────────────────────────────────────────────────────────────
static constexpr const char* NVS_NAMESPACE  = "wifi-creds";
static constexpr const char* NVS_KEY_SSID   = "ssid";
static constexpr const char* NVS_KEY_PASS   = "password";

// ── Event group bits ────────────────────────────────────────────────────────
static constexpr int CONNECTED_BIT = BIT0;

// ── Static state ────────────────────────────────────────────────────────────
static bool                 s_connected      = false;
static bool                 s_apActive       = false;
static bool                 s_wifiScanning   = false;
static EventGroupHandle_t   s_wifiEventGroup = nullptr;
static esp_netif_t*         s_staNetif       = nullptr;
static esp_netif_t*         s_apNetif        = nullptr;

// Stored for deferred AP enablement
static char s_apName[32]     = {};
static char s_apPassword[64] = {};

// Credentials cache
static int8_t s_haveCreds = -1;   // -1 unknown, 0 no, 1 yes (see hasCredentials)

// Health counter: established connections lost this boot (diagnostics)
static uint32_t s_disconnects = 0;

// connect() applied credentials and no GOT_IP has landed since. Lets the
// recovery portal distinguish "joined with the NEW credentials" from the
// standing connected level, which stays true for the OLD network throughout
// a dial-initiated change window (on-device round 2, 2026-07-12).
static bool s_pendingJoin = false;

// connect() is about to drop a live association on purpose (new credentials
// over an associated STA). The disconnect handler must consume this once:
// no old-config auto-reconnect racing the new join, no diag drop count.
static bool s_expectDisconnect = false;

// Reconnect backoff: a handful of instant retries covers the common blip;
// after that, waiting is kinder to the AP and the 2.4 GHz radio than
// hammering esp_wifi_connect() on every DISCONNECTED event. Capped well
// below the WifiRecovery fallback timeouts (2/5 min), so the recovery AP
// timing is unaffected. Counter resets on GOT_IP and on explicit connect().
static uint32_t s_retryCount = 0;
static esp_timer_handle_t s_reconnectTimer = nullptr;

// ── Trial-connect (test-then-commit) ────────────────────────────────────────
// New credentials are held here and only written to NVS once GOT_IP lands.
// On deadline, the previous credentials (still untouched in NVS) are re-applied.
// Written by connect() (httpd/main task) and the event handler; drained by
// loop() on the main task. Spinlock-guarded like the other cross-task state.
static constexpr uint32_t TRIAL_TIMEOUT_MS = 30000;
static portMUX_TYPE s_trialMux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool active = false;
    bool commitPending = false;          // GOT_IP landed — loop() must persist
    char newSsid[33] = {}, newPass[65] = {};
    char oldSsid[33] = {}, oldPass[65] = {};
    bool haveOld = false;
    int64_t deadlineMs = 0;
    WifiManager::WifiTrialState result = WifiManager::WIFI_TRIAL_IDLE;
} s_trial;

static void reconnect_timer_cb(void *) {
    esp_wifi_connect();
}

// ── Event handler ───────────────────────────────────────────────────────────
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_CONNECTED:
                LOG_INFO("STA connected to AP");
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                bool expected = s_expectDisconnect;   // deliberate drop from connect()
                s_expectDisconnect = false;
                if (s_connected && !expected) s_disconnects++;   // count real drops, not failed retries
                s_connected = false;
                if (s_wifiEventGroup) {
                    xEventGroupClearBits(s_wifiEventGroup, CONNECTED_BIT);
                }
                if (!s_wifiScanning && !expected) {
                    s_retryCount++;
                    uint32_t delayMs = 0;
                    if (s_retryCount > 3) {   // 2s, 4s, 8s, 16s, then 30s cap
                        uint32_t shift = std::min<uint32_t>(s_retryCount - 4, 4);
                        delayMs = std::min<uint32_t>(2000u << shift, 30000);
                    }
                    if (delayMs == 0) {
                        LOG_WARN("STA disconnected — reconnecting...");
                        esp_wifi_connect();
                    } else if (s_reconnectTimer &&
                               esp_timer_start_once(s_reconnectTimer, (uint64_t)delayMs * 1000) == ESP_OK) {
                        LOG_WARN("STA disconnected — retry %lu in %lums",
                                 (unsigned long)s_retryCount, (unsigned long)delayMs);
                    } else {
                        esp_wifi_connect();   // timer unavailable/already armed — retry now
                    }
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
            LOG_INFO("Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_connected = true;
            s_retryCount = 0;
            s_pendingJoin = false;
            s_expectDisconnect = false;   // never let a lost event swallow a later real drop
            portENTER_CRITICAL(&s_trialMux);
            if (s_trial.active) s_trial.commitPending = true;  // loop() persists on the main task
            portEXIT_CRITICAL(&s_trialMux);
            if (s_wifiEventGroup) {
                xEventGroupSetBits(s_wifiEventGroup, CONNECTED_BIT);
            }
        }
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

void WifiManager::init(const char* hostname, const char* apName, const char* apPassword)
{
    // Store AP credentials for later enableAP() calls
    if (apName) {
        strncpy(s_apName, apName, sizeof(s_apName) - 1);
        s_apName[sizeof(s_apName) - 1] = '\0';
    }
    if (apPassword) {
        strncpy(s_apPassword, apPassword, sizeof(s_apPassword) - 1);
        s_apPassword[sizeof(s_apPassword) - 1] = '\0';
    }

    // Create event group for waitForConnection()
    s_wifiEventGroup = xEventGroupCreate();

    // Reconnect backoff timer (see the disconnect handler)
    const esp_timer_create_args_t timerArgs = {
        .callback = reconnect_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi-retry",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timerArgs, &s_reconnectTimer);

    // Initialize TCP/IP stack and default event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default STA netif
    s_staNetif = esp_netif_create_default_wifi_sta();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    // Start in STA mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Set hostname so the router sees the device name
    if (hostname && hostname[0] != '\0') {
        ESP_ERROR_CHECK(esp_netif_set_hostname(s_staNetif, hostname));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // Disable power save — mains-powered, lower latency

    LOG_INFO("Initialized (hostname=%s, AP=%s)", hostname ? hostname : "?", s_apName);
}

// Build + apply the STA config for the given credentials and reset the retry
// budget — shared by connect() and the trial-revert path in loop() so the
// auth/PMF policy lives in one place.
static void applyStaConfig(const char *ssid, const char *password)
{
    wifi_config_t cfg = {};
    // memcpy with explicit strnlen (not strncpy): the revert caller passes
    // fixed-size arrays, which trips -Werror=stringop-truncation when inlined
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid) - 1));
    if (password && password[0] != '\0') {
        memcpy(cfg.sta.password, password, strnlen(password, sizeof(cfg.sta.password) - 1));
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    s_retryCount = 0;
}

bool WifiManager::connect(const char* ssid, const char* password)
{
    if (!ssid || ssid[0] == '\0') {
        LOG_ERROR("connect() called with empty SSID");
        return false;
    }

    // Test-then-commit: don't persist yet. If these credentials fail to join,
    // the previous (working) ones must survive in NVS — a typo in the recovery
    // portal used to orphan the device until the fallback AP reopened.
    // loop() commits after GOT_IP or reverts at the deadline.
    {
        char curSsid[33] = {0}, curPass[65] = {0};
        bool haveCur = loadCredentials(curSsid, sizeof(curSsid), curPass, sizeof(curPass));
        const char *pw = password ? password : "";
        bool sameAsStored = haveCur && strcmp(curSsid, ssid) == 0 && strcmp(curPass, pw) == 0;
        portENTER_CRITICAL(&s_trialMux);
        if (sameAsStored) {
            // Boot path / re-apply of current network: nothing to commit or revert.
            s_trial.active = false;
            s_trial.commitPending = false;
            s_trial.result = WIFI_TRIAL_IDLE;
        } else {
            s_trial.active = true;
            s_trial.commitPending = false;
            strncpy(s_trial.newSsid, ssid, sizeof(s_trial.newSsid) - 1);
            s_trial.newSsid[sizeof(s_trial.newSsid) - 1] = '\0';
            strncpy(s_trial.newPass, pw, sizeof(s_trial.newPass) - 1);
            s_trial.newPass[sizeof(s_trial.newPass) - 1] = '\0';
            s_trial.haveOld = haveCur;
            memcpy(s_trial.oldSsid, curSsid, sizeof(s_trial.oldSsid));
            memcpy(s_trial.oldPass, curPass, sizeof(s_trial.oldPass));
            s_trial.deadlineMs = (int64_t)uptime_ms() + TRIAL_TIMEOUT_MS;
            s_trial.result = WIFI_TRIAL_TESTING;
        }
        portEXIT_CRITICAL(&s_trialMux);
    }

    applyStaConfig(ssid, password);

    // Fresh credentials get a fresh retry budget — no stale delayed retry
    if (s_reconnectTimer) esp_timer_stop(s_reconnectTimer);

    // esp_wifi_connect() on an associated STA is NOT a network switch — the
    // IDF requires esp_wifi_disconnect() first. During the dial's change
    // window the STA is still up, and without this the new credentials were
    // saved but silently never applied (on-device round 2, 2026-07-12).
    s_pendingJoin = true;
    if (s_connected) {
        s_expectDisconnect = true;
        esp_wifi_disconnect();
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        LOG_ERROR("esp_wifi_connect() failed: %s", esp_err_to_name(err));
        return false;
    }

    LOG_INFO("Connecting to SSID: %s", ssid);
    return true;
}

bool WifiManager::isConnected()
{
    return s_connected;
}

WifiManager::WifiTrialState WifiManager::getTrialState()
{
    portENTER_CRITICAL(&s_trialMux);
    WifiTrialState r = s_trial.result;
    portEXIT_CRITICAL(&s_trialMux);
    return r;
}

void WifiManager::loop()
{
    // Trials are rare and their deadline is 30 s — a 250 ms gate keeps the
    // spinlock (interrupts-off window) off the ~10 ms main-loop tick that
    // runs for the device's whole life.
    uint32_t nowMs = uptime_ms();
    static uint32_t s_lastTrialCheck = 0;
    if (nowMs - s_lastTrialCheck < 250) return;
    s_lastTrialCheck = nowMs;

    // Snapshot under the lock; NVS/esp_wifi work happens outside it. The
    // credential copies matter: a concurrent connect() may overwrite
    // s_trial.* the moment the lock is released.
    portENTER_CRITICAL(&s_trialMux);
    bool active = s_trial.active;
    bool commit = s_trial.commitPending;
    bool expired = active && !commit && (int64_t)nowMs >= s_trial.deadlineMs;
    bool haveOld = s_trial.haveOld;
    char newSsid[sizeof(s_trial.newSsid)], newPass[sizeof(s_trial.newPass)];
    char oldSsid[sizeof(s_trial.oldSsid)], oldPass[sizeof(s_trial.oldPass)];
    memcpy(newSsid, s_trial.newSsid, sizeof(newSsid));
    memcpy(newPass, s_trial.newPass, sizeof(newPass));
    memcpy(oldSsid, s_trial.oldSsid, sizeof(oldSsid));
    memcpy(oldPass, s_trial.oldPass, sizeof(oldPass));
    portEXIT_CRITICAL(&s_trialMux);
    if (!active) return;

    if (commit) {
        saveCredentials(newSsid, newPass);
        portENTER_CRITICAL(&s_trialMux);
        s_trial.active = false;
        s_trial.commitPending = false;
        s_trial.result = WIFI_TRIAL_SUCCESS;
        portEXIT_CRITICAL(&s_trialMux);
        LOG_INFO("Trial join succeeded — credentials committed (SSID: %s)", newSsid);
        return;
    }

    if (expired) {
        portENTER_CRITICAL(&s_trialMux);
        s_trial.active = false;
        s_trial.result = WIFI_TRIAL_FAILED;  // latched until the next connect()
        portEXIT_CRITICAL(&s_trialMux);
        s_pendingJoin = false;
        if (haveOld) {
            LOG_WARN("Trial join to '%s' failed — restoring previous network '%s'",
                     newSsid, oldSsid);
            applyStaConfig(oldSsid, oldPass);
            esp_wifi_connect();
        } else {
            LOG_WARN("Trial join to '%s' failed — no previous credentials to restore",
                     newSsid);
        }
    }
}

bool WifiManager::isJoinPending()
{
    return s_pendingJoin;
}

bool WifiManager::waitForConnection(uint32_t timeoutMs)
{
    if (s_connected) return true;
    if (!s_wifiEventGroup) return false;

    EventBits_t bits = xEventGroupWaitBits(
        s_wifiEventGroup,
        CONNECTED_BIT,
        pdFALSE,           // Don't clear on exit
        pdTRUE,            // Wait for all bits (only one here)
        pdMS_TO_TICKS(timeoutMs)
    );

    return (bits & CONNECTED_BIT) != 0;
}

uint32_t WifiManager::getDisconnectCount()
{
    return s_disconnects;
}

const char* WifiManager::getHostname()
{
    return s_apName;
}

int8_t WifiManager::getRSSI()
{
    if (!s_connected) return 0;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

void WifiManager::getSSID(char* out, size_t len)
{
    if (!out || !len) return;
    out[0] = '\0';
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK)
        strncpy(out, (const char*)cfg.sta.ssid, len - 1);
}

void WifiManager::getIP(char* out, size_t len)
{
    if (!out || !len) return;
    out[0] = '\0';
    esp_netif_ip_info_t ip;
    if (s_staNetif && esp_netif_get_ip_info(s_staNetif, &ip) == ESP_OK)
        esp_ip4addr_ntoa(&ip.ip, out, len);
}

void WifiManager::enableAP(const char* apName, const char* apPassword)
{
    if (s_apActive) return;

    // Create AP netif lazily (only once)
    if (!s_apNetif) {
        s_apNetif = esp_netif_create_default_wifi_ap();
    }

    // Switch to STA+AP mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure AP
    wifi_config_t ap_config = {};
    strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), apName,
            sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = static_cast<uint8_t>(strlen(apName));

    if (apPassword && apPassword[0] != '\0') {
        strncpy(reinterpret_cast<char*>(ap_config.ap.password), apPassword,
                sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_config.ap.channel         = 1;
    ap_config.ap.max_connection  = 4;
    ap_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    s_apActive = true;
    LOG_INFO("AP enabled: %s", apName);
}

void WifiManager::disableAP()
{
    if (!s_apActive) return;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_apActive = false;
    LOG_INFO("AP disabled");
}

// ── NVS credential management ──────────────────────────────────────────────

bool WifiManager::loadCredentials(char* ssid, size_t ssidLen, char* password, size_t passLen)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        LOG_DEBUG("No saved credentials (nvs_open: %s)", esp_err_to_name(err));
        return false;
    }

    bool found = false;
    size_t len = ssidLen;
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &len);
    if (err == ESP_OK && len > 1) {  // len includes null terminator
        len = passLen;
        err = nvs_get_str(handle, NVS_KEY_PASS, password, &len);
        if (err == ESP_OK) {
            found = true;
        } else {
            // SSID found but no password — connect with empty password
            if (password && passLen > 0) password[0] = '\0';
            found = true;
        }
    }

    nvs_close(handle);
    return found;
}

void WifiManager::saveCredentials(const char* ssid, const char* password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        LOG_ERROR("Failed to open NVS for writing: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASS, password ? password : "");
    nvs_commit(handle);
    nvs_close(handle);

    s_haveCreds = 1;
    LOG_INFO("Credentials saved (SSID: %s)", ssid);
}

void WifiManager::eraseCredentials()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        LOG_ERROR("Failed to open NVS for erase: %s", esp_err_to_name(err));
        return;
    }

    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);

    s_haveCreds = 0;
    LOG_WARN("Credentials erased");
}

// ── WiFi scanning ──────────────────────────────────────────────────────────

// Resume STA connection after scan if credentials are configured
static void resumeStaConnection()
{
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0') {
        LOG_DEBUG("Resuming STA connection after scan");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            LOG_WARN("Failed to resume connection: %s", esp_err_to_name(err));
        }
    }
}

int WifiManager::scanNetworks(ScannedNetwork* results, int maxResults)
{
    // If STA is connected, scan without disconnecting (ESP-IDF supports
    // scanning while associated — the radio briefly hops channels and returns).
    // Only disconnect when not connected (recovery/AP mode) to free the radio.
    bool wasConnected = s_connected;
    if (!wasConnected) {
        s_wifiScanning = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    wifi_scan_config_t scanConf = {};
    scanConf.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&scanConf, true);  // blocking scan
    if (!wasConnected) s_wifiScanning = false;
    if (err != ESP_OK) {
        LOG_ERROR("Scan failed: %s", esp_err_to_name(err));
        if (!wasConnected) resumeStaConnection();
        return 0;
    }

    uint16_t apCount = 0;
    esp_wifi_scan_get_ap_num(&apCount);
    if (apCount == 0) {
        if (!wasConnected) resumeStaConnection();
        return 0;
    }

    uint16_t fetchCount = (apCount > 15) ? 15 : apCount;
    wifi_ap_record_t apRecords[15];
    esp_wifi_scan_get_ap_records(&fetchCount, apRecords);

    // Deduplicate by SSID, keeping the strongest signal
    int count = 0;
    for (int i = 0; i < fetchCount && count < maxResults; i++) {
        if (apRecords[i].ssid[0] == '\0') continue;  // skip hidden networks

        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(results[j].ssid, (const char *)apRecords[i].ssid) == 0) {
                if (apRecords[i].rssi > results[j].rssi)
                    results[j].rssi = apRecords[i].rssi;
                dup = true;
                break;
            }
        }
        if (dup) continue;

        strncpy(results[count].ssid, (const char *)apRecords[i].ssid, 32);
        results[count].ssid[32] = '\0';
        results[count].rssi = apRecords[i].rssi;
        results[count].secure = (apRecords[i].authmode != WIFI_AUTH_OPEN);
        count++;
    }

    // Sort by RSSI descending (strongest first)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (results[j].rssi > results[i].rssi) {
                ScannedNetwork tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    LOG_INFO("Scan found %d unique networks", count);
    if (!wasConnected) resumeStaConnection();
    return count;
}

bool WifiManager::hasCredentials()
{
    if (s_haveCreds < 0) {
        char ssid[33], pass[65];
        s_haveCreds = loadCredentials(ssid, sizeof(ssid), pass, sizeof(pass)) ? 1 : 0;
        memset(pass, 0, sizeof(pass));
    }
    return s_haveCreds == 1;
}
