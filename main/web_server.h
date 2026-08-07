#pragma once

#include <atomic>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "cn105_protocol.h"
#include "settings.h"

class WebUI {
public:
    void begin(CN105Controller *ctrl);
    void loop();                        // Called from main loop to push state updates
    void broadcastLog(const char *msg, size_t len); // Send log line to WS client
    void setAPMode(bool active);        // Toggle AP mode flag (controls page routing)
    bool isAPMode() const { return _apMode; }

private:
    httpd_handle_t   _server = NULL;
    CN105Controller *_ctrl   = nullptr;
    uint32_t _lastStatePush  = 0;
    bool _apMode = false;               // True when fallback AP is active
    SemaphoreHandle_t _wsSendMux = nullptr;  // Serializes all WS frame writes (see sendWsText)

    void applyCaptivePortalHandler();   // (Un)install the AP-mode captive 404 handler

    // ── HTTP handlers (static, access global webUI instance) ────────────────
    static esp_err_t handleRoot(httpd_req_t *req);
    static esp_err_t handleRecoveryPage(httpd_req_t *req);
    static esp_err_t handleWifiStatus(httpd_req_t *req);
    static esp_err_t handleWifiSetup(httpd_req_t *req);
    static esp_err_t handleWifiScan(httpd_req_t *req);
    static esp_err_t handleIdentify(httpd_req_t *req);
    static esp_err_t handleOtaUpload(httpd_req_t *req);
    static esp_err_t handleWebSocket(httpd_req_t *req);
    static esp_err_t handleManifest(httpd_req_t *req);
    static esp_err_t handleIcon192(httpd_req_t *req);
    static esp_err_t handleIcon512(httpd_req_t *req);
    static esp_err_t handleFavicon(httpd_req_t *req);

    // ── WebSocket message handling ──────────────────────────────────────────
    void handleWsMessage(httpd_req_t *req, const char *msg);
    void sendDeviceInfo(int fd);        // one-shot identity/diagnostics frame on WS connect
    void pushState();
    void pushDiscoveryResults(bool done);
    void sendWsText(int fd, const char *text);
    void broadcastWs(const char *text);          // Send to every WS client, refreshing LRU
    int  collectWsClients(int *out, int maxOut); // List active WebSocket client fds

    // ── WiFi credential handling (shared by REST + WS paths) ────────────────
    bool applyWifiCredentials(const char *json, const char **outError);

    // ── Helpers ─────────────────────────────────────────────────────────────
    static esp_err_t sendGzipPage(httpd_req_t *req, const uint8_t *data, size_t len);
};

extern WebUI webUI;

bool webota_active();   // web_ota.cpp — OTA upload in flight
