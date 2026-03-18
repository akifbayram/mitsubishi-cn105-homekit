#include "wifi_manager.h"
#include "logging.h"

#include <cstring>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
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
static EventGroupHandle_t   s_wifiEventGroup = nullptr;
static esp_netif_t*         s_staNetif       = nullptr;
static esp_netif_t*         s_apNetif        = nullptr;

// Stored for deferred AP enablement
static char s_apName[32]     = {};
static char s_apPassword[64] = {};

// ── Event handler ───────────────────────────────────────────────────────────
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_CONNECTED:
                LOG_INFO("[WiFi] STA connected to AP");
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                s_connected = false;
                if (s_wifiEventGroup) {
                    xEventGroupClearBits(s_wifiEventGroup, CONNECTED_BIT);
                }
                LOG_WARN("[WiFi] STA disconnected — reconnecting...");
                esp_wifi_connect();
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
            LOG_INFO("[WiFi] Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_connected = true;
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

    LOG_INFO("[WiFi] Initialized (hostname=%s, AP=%s)", hostname ? hostname : "?", s_apName);
}

bool WifiManager::connect(const char* ssid, const char* password)
{
    if (!ssid || ssid[0] == '\0') {
        LOG_ERROR("[WiFi] connect() called with empty SSID");
        return false;
    }

    // Save credentials to NVS
    saveCredentials(ssid, password);

    // Configure STA
    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    if (password && password[0] != '\0') {
        strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password,
                sizeof(wifi_config.sta.password) - 1);
    }
    // Use WPA2/WPA3 threshold for security
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable    = true;
    wifi_config.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        LOG_ERROR("[WiFi] esp_wifi_connect() failed: %s", esp_err_to_name(err));
        return false;
    }

    LOG_INFO("[WiFi] Connecting to SSID: %s", ssid);
    return true;
}

bool WifiManager::isConnected()
{
    return s_connected;
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

int8_t WifiManager::getRSSI()
{
    if (!s_connected) return 0;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
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
    LOG_INFO("[WiFi] AP enabled: %s", apName);
}

void WifiManager::disableAP()
{
    if (!s_apActive) return;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_apActive = false;
    LOG_INFO("[WiFi] AP disabled");
}

bool WifiManager::isAPActive()
{
    return s_apActive;
}

// ── NVS credential management ──────────────────────────────────────────────

bool WifiManager::loadCredentials(char* ssid, size_t ssidLen, char* password, size_t passLen)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        LOG_DEBUG("[WiFi] No saved credentials (nvs_open: %s)", esp_err_to_name(err));
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
        LOG_ERROR("[WiFi] Failed to open NVS for writing: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASS, password ? password : "");
    nvs_commit(handle);
    nvs_close(handle);

    LOG_INFO("[WiFi] Credentials saved (SSID: %s)", ssid);
}

void WifiManager::eraseCredentials()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        LOG_ERROR("[WiFi] Failed to open NVS for erase: %s", esp_err_to_name(err));
        return;
    }

    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);

    LOG_WARN("[WiFi] Credentials erased");
}
