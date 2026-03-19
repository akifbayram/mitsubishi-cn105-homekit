#pragma once

#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include "cn105_protocol.h"
#include "settings.h"

class WebUI {
public:
    void begin(CN105Controller *ctrl);
    void loop();                        // Called from main loop to push state updates
    void broadcastLog(const char *msg, size_t len); // Send log line to WS client
    void setAPMode(bool active);        // Toggle AP mode flag (controls page routing)

private:
    httpd_handle_t   _server = NULL;
    CN105Controller *_ctrl   = nullptr;
    int  _wsClientFd         = -1;      // Track connected WS client (single client)
    uint32_t _lastStatePush  = 0;
    bool _apMode = false;               // True when fallback AP is active

    httpd_handle_t   _redirectServer = NULL;  // Port 80 redirect server for AP mode

    // ── HTTP handlers (static, access global webUI instance) ────────────────
    static esp_err_t handleRoot(httpd_req_t *req);
    static esp_err_t handleRecoveryPage(httpd_req_t *req);
    static esp_err_t handleRedirect80(httpd_req_t *req);
    static esp_err_t handleWifiStatus(httpd_req_t *req);
    static esp_err_t handleWifiSetup(httpd_req_t *req);
    static esp_err_t handleWifiScan(httpd_req_t *req);
    static esp_err_t handleOtaUpload(httpd_req_t *req);
    static esp_err_t handleWebSocket(httpd_req_t *req);
    static esp_err_t handleManifest(httpd_req_t *req);
    static esp_err_t handleIcon192(httpd_req_t *req);
    static esp_err_t handleIcon512(httpd_req_t *req);
    static esp_err_t handleFavicon(httpd_req_t *req);

    // ── WebSocket message handling ──────────────────────────────────────────
    void handleWsMessage(httpd_req_t *req, const char *msg);
    void pushState();
    void pushDiscoveryResults(bool done);
    void sendWsText(int fd, const char *text);

    // ── WiFi credential handling (shared by REST + WS paths) ────────────────
    bool applyWifiCredentials(const char *json, const char **outError);

    // ── Helpers ─────────────────────────────────────────────────────────────
    static esp_err_t sendGzipPage(httpd_req_t *req, const uint8_t *data, size_t len);
};

extern WebUI webUI;
