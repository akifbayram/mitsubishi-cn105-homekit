#pragma once
#include <Arduino.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <HomeSpan.h>
#include "cn105_protocol.h"
#include "settings.h"

class WebUI {
public:
    void begin(CN105Controller *ctrl);
    void loop();                        // Called from main loop to push state updates
    void broadcastLog(const char *msg); // Send log line to all WS clients
    void setSetupCode(const char *code); // Store setup code for web UI display
    void setQRID(const char *id);         // Store QR code setup ID
    void setAPMode(bool active);         // Toggle AP mode flag (controls page routing)

private:
    httpd_handle_t   _server = NULL;
    CN105Controller *_ctrl   = nullptr;
    int  _wsClientFd         = -1;      // Track connected WS client (single client)
    uint32_t _lastStatePush  = 0;
    HS_STATUS _hkStatus = HS_WIFI_NEEDED;  // Updated by HomeSpan callback
    char _setupCode[12] = "";               // Pairing code for web UI display
    char _qrID[5] = "HSPN";                // QR code setup ID
    char _fmtCode[12] = "";                 // Formatted setup code (XXX-XX-XXX), set by updateCachedSetupInfo
    char _setupURI[22] = "";                // Cached X-HM:// URI for QR code
    bool _apMode = false;                   // True when fallback AP is active

    httpd_handle_t   _redirectServer = NULL;  // Port 80 redirect server for AP mode

    static esp_err_t handleRoot(httpd_req_t *req);
    static esp_err_t handleRecoveryPage(httpd_req_t *req);
    static esp_err_t handleRedirect80(httpd_req_t *req);
    static esp_err_t handleWifiStatus(httpd_req_t *req);
    static esp_err_t handleWifiSetup(httpd_req_t *req);
    static esp_err_t handleOtaUpload(httpd_req_t *req);
    static esp_err_t handleWebSocket(httpd_req_t *req);
    static esp_err_t handleManifest(httpd_req_t *req);
    static esp_err_t handleIcon192(httpd_req_t *req);
    static esp_err_t handleIcon512(httpd_req_t *req);
    void handleWsMessage(httpd_req_t *req, const char *msg);
    void pushState();
    void pushDiscoveryResults(bool done);
    void sendWsText(int fd, const char *text);
    void updateCachedSetupInfo();            // Recompute _fmtCode and _setupURI
    bool applyWifiCredentials(const char *json, const char **outError); // Shared WiFi cred logic

    static esp_err_t sendGzipPage(httpd_req_t *req, const uint8_t *data, size_t len);
};

extern WebUI webUI;
