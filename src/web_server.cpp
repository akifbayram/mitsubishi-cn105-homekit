#include "web_server.h"
#include "web_ui_html.h"
#include "wifi_recovery_html.h"
#include "wifi_recovery.h"
#include <WiFi.h>
#include <cstring>
#include <cstdio>
#include "HomeSpan.h"
#include <mbedtls/sha256.h>
#include <lwip/sockets.h>
#include "status_led.h"

extern StatusLED statusLED;

// ── Global instance ──────────────────────────────────────────────────────────
WebUI webUI;
LogHookFn logHook = nullptr;

// ══════════════════════════════════════════════════════════════════════════════
// Mode / Fan / Vane string conversions
// ══════════════════════════════════════════════════════════════════════════════

const char *WebUI::modeToStr(uint8_t mode) {
    switch (mode) {
        case CN105_MODE_HEAT: return "heat";
        case CN105_MODE_DRY:  return "dry";
        case CN105_MODE_COOL: return "cool";
        case CN105_MODE_FAN:  return "fan";
        case CN105_MODE_AUTO: return "auto";
        default:              return "unknown";
    }
}

const char *WebUI::fanToStr(uint8_t fan) {
    switch (fan) {
        case CN105_FAN_AUTO:  return "auto";
        case CN105_FAN_QUIET: return "quiet";
        case CN105_FAN_1:     return "1";
        case CN105_FAN_2:     return "2";
        case CN105_FAN_3:     return "3";
        case CN105_FAN_4:     return "4";
        default:              return "unknown";
    }
}

const char *WebUI::vaneToStr(uint8_t vane) {
    switch (vane) {
        case CN105_VANE_AUTO:  return "auto";
        case CN105_VANE_1:     return "1";
        case CN105_VANE_2:     return "2";
        case CN105_VANE_3:     return "3";
        case CN105_VANE_4:     return "4";
        case CN105_VANE_5:     return "5";
        case CN105_VANE_SWING: return "swing";
        default:               return "unknown";
    }
}

uint8_t WebUI::strToMode(const char *s) {
    if (strcmp(s, "heat") == 0) return CN105_MODE_HEAT;
    if (strcmp(s, "dry")  == 0) return CN105_MODE_DRY;
    if (strcmp(s, "cool") == 0) return CN105_MODE_COOL;
    if (strcmp(s, "fan")  == 0) return CN105_MODE_FAN;
    if (strcmp(s, "auto") == 0) return CN105_MODE_AUTO;
    return CN105_MODE_AUTO;  // fallback
}

uint8_t WebUI::strToFan(const char *s) {
    if (strcmp(s, "auto")  == 0) return CN105_FAN_AUTO;
    if (strcmp(s, "quiet") == 0) return CN105_FAN_QUIET;
    if (strcmp(s, "1")     == 0) return CN105_FAN_1;
    if (strcmp(s, "2")     == 0) return CN105_FAN_2;
    if (strcmp(s, "3")     == 0) return CN105_FAN_3;
    if (strcmp(s, "4")     == 0) return CN105_FAN_4;
    return CN105_FAN_AUTO;  // fallback
}

uint8_t WebUI::strToVane(const char *s) {
    if (strcmp(s, "auto")  == 0) return CN105_VANE_AUTO;
    if (strcmp(s, "1")     == 0) return CN105_VANE_1;
    if (strcmp(s, "2")     == 0) return CN105_VANE_2;
    if (strcmp(s, "3")     == 0) return CN105_VANE_3;
    if (strcmp(s, "4")     == 0) return CN105_VANE_4;
    if (strcmp(s, "5")     == 0) return CN105_VANE_5;
    if (strcmp(s, "swing") == 0) return CN105_VANE_SWING;
    return CN105_VANE_AUTO;  // fallback
}

const char *WebUI::wideVaneToStr(uint8_t wv) {
    switch (wv) {
        case CN105_WVANE_LEFT_LEFT:   return "ll";
        case CN105_WVANE_LEFT:        return "l";
        case CN105_WVANE_CENTER:      return "c";
        case CN105_WVANE_RIGHT:       return "r";
        case CN105_WVANE_RIGHT_RIGHT: return "rr";
        case CN105_WVANE_SPLIT:       return "split";
        case CN105_WVANE_SWING:       return "swing";
        default:                      return "unknown";
    }
}

uint8_t WebUI::strToWideVane(const char *s) {
    if (strcmp(s, "ll")    == 0) return CN105_WVANE_LEFT_LEFT;
    if (strcmp(s, "l")     == 0) return CN105_WVANE_LEFT;
    if (strcmp(s, "c")     == 0) return CN105_WVANE_CENTER;
    if (strcmp(s, "r")     == 0) return CN105_WVANE_RIGHT;
    if (strcmp(s, "rr")    == 0) return CN105_WVANE_RIGHT_RIGHT;
    if (strcmp(s, "split") == 0) return CN105_WVANE_SPLIT;
    if (strcmp(s, "swing") == 0) return CN105_WVANE_SWING;
    return CN105_WVANE_CENTER;
}

// ── Sub mode / Stage / Auto sub mode string conversions (file-local) ─────
static const char* webSubModeToStr(uint8_t sm) {
    switch (sm) {
        case 0x00: return "normal";
        case 0x02: return "defrost";
        case 0x04: return "preheat";
        case 0x08: return "standby";
        default:   return "unknown";
    }
}

static const char* webStageToStr(uint8_t st) {
    switch (st) {
        case 0x00: return "idle";
        case 0x01: return "low";
        case 0x02: return "gentle";
        case 0x03: return "medium";
        case 0x04: return "moderate";
        case 0x05: return "high";
        case 0x06: return "diffuse";
        default:   return "unknown";
    }
}

static const char* webAutoSubModeToStr(uint8_t asm_) {
    switch (asm_) {
        case 0x00: return "off";
        case 0x01: return "cool";
        case 0x02: return "heat";
        case 0x03: return "leader";
        default:   return "unknown";
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// JSON parsing helpers (simple strstr-based, no ArduinoJson)
// ══════════════════════════════════════════════════════════════════════════════

// Extract a string value for a given key from JSON.
// Returns true if found; copies value into buf (up to bufLen-1 chars).
static bool jsonGetString(const char *json, const char *key, char *buf, size_t bufLen) {
    // Build search pattern: "key":"
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = end - p;
    if (len >= bufLen) len = bufLen - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return true;
}

// Extract a numeric (float) value for a given key from JSON.
// Returns true if found.
static bool jsonGetFloat(const char *json, const char *key, float *out) {
    // Build search pattern: "key":
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    // Skip whitespace
    while (*p == ' ') p++;
    char *endp;
    float val = strtof(p, &endp);
    if (endp == p) return false;
    *out = val;
    return true;
}

// Extract an integer value for a given key from JSON.
// Returns true if found.
static bool jsonGetInt(const char *json, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    char *endp;
    long val = strtol(p, &endp, 10);
    if (endp == p) return false;
    *out = (int)val;
    return true;
}

// Extract a boolean value for a given key from JSON.
// Returns true if found.
static bool jsonGetBool(const char *json, const char *key, bool *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// HTTP handler: GET /  (serve embedded gzipped HTML)
// ══════════════════════════════════════════════════════════════════════════════

esp_err_t WebUI::sendGzipPage(httpd_req_t *req, const uint8_t *data, size_t len) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)data, len);
    return ESP_OK;
}

esp_err_t WebUI::handleRoot(httpd_req_t *req) {
    // If AP fallback is active, check if client is on the AP subnet (192.168.4.x)
    if (webUI._apMode) {
        int fd = httpd_req_to_sockfd(req);
        struct sockaddr_storage saddr;
        socklen_t addrLen = sizeof(saddr);
        if (getpeername(fd, (struct sockaddr *)&saddr, &addrLen) == 0) {
            uint32_t ip = 0;
            if (saddr.ss_family == AF_INET) {
                ip = ntohl(((struct sockaddr_in *)&saddr)->sin_addr.s_addr);
            } else if (saddr.ss_family == AF_INET6) {
                // IPv4-mapped IPv6: ::ffff:x.x.x.x — last 4 bytes are the IPv4 address
                struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&saddr;
                uint8_t *b = a6->sin6_addr.s6_addr;
                if (b[10] == 0xFF && b[11] == 0xFF) {
                    ip = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                         ((uint32_t)b[14] << 8)  |  (uint32_t)b[15];
                }
            }
            // AP subnet: 192.168.4.0/24 = 0xC0A80400
            if (ip && (ip & 0xFFFFFF00) == 0xC0A80400) {
                return handleRecoveryPage(req);
            }
        }
    }

    return sendGzipPage(req, WEB_UI_HTML_GZ, WEB_UI_HTML_GZ_LEN);
}

esp_err_t WebUI::handleRecoveryPage(httpd_req_t *req) {
    return sendGzipPage(req, WIFI_RECOVERY_HTML_GZ, WIFI_RECOVERY_HTML_GZ_LEN);
}

static constexpr char AP_REDIRECT_URL[] = "http://192.168.4.1:8080/";

static void doRedirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", AP_REDIRECT_URL);
    httpd_resp_sendstr(req, "Redirecting...");
}

esp_err_t WebUI::handleRedirect80(httpd_req_t *req) {
    doRedirect(req);
    return ESP_OK;
}

static esp_err_t handleRedirect404(httpd_req_t *req, httpd_err_code_t) {
    doRedirect(req);
    return ESP_OK;
}

// Escape a string for safe embedding in a JSON string literal
static size_t jsonEscape(const char *src, char *dst, size_t dstLen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstLen - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j < dstLen - 3) { dst[j++] = '\\'; dst[j++] = src[i]; }
        } else if (src[i] == '\n') {
            if (j < dstLen - 3) { dst[j++] = '\\'; dst[j++] = 'n'; }
        } else if (src[i] == '\r') {
            // skip carriage returns
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

esp_err_t WebUI::handleWifiStatus(httpd_req_t *req) {
    char ssid[33] = "";
    wifiRecovery.getCachedSSID(ssid, sizeof(ssid));

    const DeviceSettings &cfg = settings.get();
    bool connected = (WiFi.status() == WL_CONNECTED);

    char escSSID[67], escName[65];
    jsonEscape(ssid, escSSID, sizeof(escSSID));
    jsonEscape(cfg.deviceName, escName, sizeof(escName));

    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"ssid\":\"%s\",\"deviceName\":\"%s\",\"connected\":%s}",
        escSSID, escName, connected ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

bool WebUI::applyWifiCredentials(const char *json, const char **outError) {
    char ssid[33] = {0};
    char password[65] = {0};
    if (!jsonGetString(json, "ssid", ssid, sizeof(ssid)) || strlen(ssid) == 0) {
        *outError = "SSID is required";
        return false;
    }
    if (!jsonGetString(json, "password", password, sizeof(password)) || strlen(password) == 0) {
        *outError = "Password is required";
        return false;
    }
    LOG_INFO("[WebUI] Saving WiFi credentials (SSID: %s)", ssid);
    wifiRecovery.setChangePending(true);
    homeSpan.setWifiCredentials(ssid, password);
    return true;
}

esp_err_t WebUI::handleWifiSetup(httpd_req_t *req) {
    char body[150] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }
    body[received] = '\0';

    const char *error = nullptr;
    if (!webUI.applyWifiCredentials(body, &error)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"saved\"}");
    return ESP_OK;
}

void WebUI::setAPMode(bool active) {
    if (active == _apMode) return;
    _apMode = active;

    if (active && !_redirectServer) {
        // Start lightweight HTTP server on port 80 for captive portal redirect
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = 80;
        cfg.ctrl_port   = 32770;
        cfg.stack_size  = 4096;
        cfg.max_open_sockets = 2;
        cfg.lru_purge_enable = true;
        if (httpd_start(&_redirectServer, &cfg) == ESP_OK) {
            // Catch root
            const httpd_uri_t rUri = {
                .uri = "/", .method = HTTP_GET, .handler = handleRedirect80,
                .user_ctx = this, .is_websocket = false,
                .handle_ws_control_frames = false, .supported_subprotocol = NULL
            };
            httpd_register_uri_handler(_redirectServer, &rUri);
            // Catch all other URIs (OS captive portal checks: /generate_204, /connecttest.txt, etc.)
            httpd_register_err_handler(_redirectServer, HTTPD_404_NOT_FOUND, handleRedirect404);
            LOG_INFO("[WebUI] Port 80 captive portal server started");
        }
    } else if (!active && _redirectServer) {
        httpd_stop(_redirectServer);
        _redirectServer = NULL;
        LOG_INFO("[WebUI] Port 80 redirect server stopped");
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// OTA firmware upload: POST /upload  (raw binary body, not multipart)
// ══════════════════════════════════════════════════════════════════════════════

esp_err_t WebUI::handleOtaUpload(httpd_req_t *req) {
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        LOG_ERROR("[OTA] No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    size_t totalLen = req->content_len;
    if (totalLen == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }
    if (totalLen > partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware too large");
        return ESP_FAIL;
    }

    LOG_INFO("[OTA] Starting firmware upload: %u bytes -> partition '%s'",
             totalLen, partition->label);

    // Notify WS client
    char otaMsg[128];
    snprintf(otaMsg, sizeof(otaMsg),
        "{\"type\":\"ota\",\"status\":\"starting\",\"size\":%u}", totalLen);
    webUI.sendWsText(webUI._wsClientFd, otaMsg);

    esp_ota_handle_t otaHandle;
    esp_err_t err = esp_ota_begin(partition, totalLen, &otaHandle);
    if (err != ESP_OK) {
        LOG_ERROR("[OTA] esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    statusLED.setState(SLED_OTA);

    // SHA256 verification: check for X-Firmware-SHA256 header
    char expectedHash[65] = {0};
    bool verifyHash = false;
    {
        size_t hdrLen = httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256");
        if (hdrLen == 64) {
            httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", expectedHash, sizeof(expectedHash));
            verifyHash = true;
            LOG_INFO("[OTA] SHA256 verification enabled");
        }
    }

    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);  // 0 = SHA-256 (not SHA-224)

    // Stream firmware in chunks
    char *buf = (char *)malloc(4096);
    if (!buf) {
        mbedtls_sha256_free(&sha256_ctx);
        esp_ota_abort(otaHandle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    bool firstChunk = true;
    while (received < totalLen) {
        int ret = httpd_req_recv(req, buf, 4096);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            LOG_ERROR("[OTA] Receive error at %u/%u bytes", received, totalLen);
            mbedtls_sha256_free(&sha256_ctx);
            free(buf);
            esp_ota_abort(otaHandle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        // Validate magic byte on first chunk
        if (firstChunk) {
            if ((uint8_t)buf[0] != 0xE9) {
                LOG_ERROR("[OTA] Invalid firmware magic byte: 0x%02X (expected 0xE9)",
                          (uint8_t)buf[0]);
                mbedtls_sha256_free(&sha256_ctx);
                free(buf);
                esp_ota_abort(otaHandle);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware file");
                return ESP_FAIL;
            }
            firstChunk = false;
        }

        err = esp_ota_write(otaHandle, buf, ret);
        if (err != ESP_OK) {
            LOG_ERROR("[OTA] esp_ota_write failed at %u bytes: %s",
                      received, esp_err_to_name(err));
            mbedtls_sha256_free(&sha256_ctx);
            free(buf);
            esp_ota_abort(otaHandle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        received += ret;
        mbedtls_sha256_update(&sha256_ctx, (const unsigned char *)buf, ret);

        // Progress update every ~64KB
        if ((received % 65536) < 4096) {
            uint8_t pct = (uint8_t)((received * 100) / totalLen);
            LOG_INFO("[OTA] Progress: %u/%u bytes (%u%%)", received, totalLen, pct);
            snprintf(otaMsg, sizeof(otaMsg),
                "{\"type\":\"ota\",\"status\":\"progress\",\"pct\":%u}", pct);
            webUI.sendWsText(webUI._wsClientFd, otaMsg);
        }
    }

    free(buf);

    // Finalize SHA256
    unsigned char hash[32];
    mbedtls_sha256_finish(&sha256_ctx, hash);
    mbedtls_sha256_free(&sha256_ctx);

    if (verifyHash) {
        char computed[65];
        for (int i = 0; i < 32; i++) {
            sprintf(computed + i * 2, "%02x", hash[i]);
        }
        computed[64] = '\0';

        if (strcmp(computed, expectedHash) != 0) {
            LOG_ERROR("[OTA] SHA256 mismatch! Expected: %.16s... Got: %.16s...",
                      expectedHash, computed);
            esp_ota_abort(otaHandle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SHA256 mismatch");
            return ESP_FAIL;
        }
        LOG_INFO("[OTA] SHA256 verified: %.16s...", computed);
    }

    err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
        LOG_ERROR("[OTA] esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        LOG_ERROR("[OTA] esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    LOG_INFO("[OTA] Firmware update successful (%u bytes). Restarting...", received);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");

    snprintf(otaMsg, sizeof(otaMsg),
        "{\"type\":\"ota\",\"status\":\"done\",\"pct\":100}");
    webUI.sendWsText(webUI._wsClientFd, otaMsg);

    delay(1000);
    ESP.restart();
    return ESP_OK;
}

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket handler: GET /ws
// ══════════════════════════════════════════════════════════════════════════════

esp_err_t WebUI::handleWebSocket(httpd_req_t *req) {
    // On first call (handshake), req->method == HTTP_GET
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        LOG_INFO("[WebUI] WebSocket client connected (fd=%d)", fd);
        webUI._wsClientFd = fd;
        // Push initial state immediately after connection
        webUI.pushState();
        return ESP_OK;
    }

    // Receive WebSocket frame
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    // First call with max_len=0 to get the frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        LOG_ERROR("[WebUI] httpd_ws_recv_frame (len query) failed: %d", ret);
        return ret;
    }

    if (frame.len == 0) {
        // Empty frame or control frame (CLOSE/PING/PONG)
        if (frame.type == HTTPD_WS_TYPE_CLOSE) {
            LOG_INFO("[WebUI] WebSocket client disconnected");
            webUI._wsClientFd = -1;
        }
        return ESP_OK;
    }

    // Allocate buffer and receive payload
    if (frame.len > 1024) {
        LOG_WARN("[WebUI] WS frame too large (%d bytes), ignoring", frame.len);
        return ESP_OK;
    }

    uint8_t *buf = (uint8_t *)malloc(frame.len + 1);
    if (!buf) {
        LOG_ERROR("[WebUI] Failed to allocate WS receive buffer");
        return ESP_ERR_NO_MEM;
    }

    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        LOG_ERROR("[WebUI] httpd_ws_recv_frame (data) failed: %d", ret);
        free(buf);
        return ret;
    }

    // Null-terminate for string processing
    buf[frame.len] = '\0';

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        LOG_DEBUG("[WebUI] WS received: %s", (char *)buf);
        webUI.handleWsMessage(req, (const char *)buf);
    } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        LOG_INFO("[WebUI] WebSocket client disconnected");
        webUI._wsClientFd = -1;
    }

    free(buf);
    return ESP_OK;
}

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket message dispatcher
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::handleWsMessage(httpd_req_t *req, const char *msg) {
    char cmd[16] = {0};
    if (!jsonGetString(msg, "cmd", cmd, sizeof(cmd))) {
        LOG_WARN("[WebUI] WS message missing 'cmd' field");
        return;
    }

    if (strcmp(cmd, "set") == 0) {
        // ── Control commands ─────────────────────────────────────────────
        bool hasControlChange = false;

        bool boolVal;
        if (jsonGetBool(msg, "power", &boolVal)) {
            LOG_INFO("[WebUI] Set power=%s", boolVal ? "ON" : "OFF");
            _ctrl->setPower(boolVal);
            hasControlChange = true;
        }

        char strVal[16];
        if (jsonGetString(msg, "mode", strVal, sizeof(strVal))) {
            uint8_t mode = strToMode(strVal);
            LOG_INFO("[WebUI] Set mode=%s (0x%02X)", strVal, mode);
            _ctrl->setMode(mode);
            hasControlChange = true;
        }

        float floatVal;
        if (jsonGetFloat(msg, "target", &floatVal)) {
            LOG_INFO("[WebUI] Set target=%.1f", floatVal);
            _ctrl->setTargetTemp(floatVal);
            hasControlChange = true;
        }

        if (jsonGetString(msg, "fan", strVal, sizeof(strVal))) {
            uint8_t fan = strToFan(strVal);
            LOG_INFO("[WebUI] Set fan=%s (0x%02X)", strVal, fan);
            _ctrl->setFanSpeed(fan);
            hasControlChange = true;
        }

        if (jsonGetString(msg, "vane", strVal, sizeof(strVal))) {
            uint8_t vane = strToVane(strVal);
            LOG_INFO("[WebUI] Set vane=%s (0x%02X)", strVal, vane);
            _ctrl->setVane(vane);
            hasControlChange = true;
        }

        if (jsonGetString(msg, "wideVane", strVal, sizeof(strVal))) {
            uint8_t wv = strToWideVane(strVal);
            LOG_INFO("[WebUI] Set wideVane=%s (0x%02X)", strVal, wv);
            _ctrl->setWideVane(wv);
            hasControlChange = true;
        }

        if (hasControlChange) {
            _ctrl->sendPendingChanges();
        }

        float heatT, coolT;
        bool threshChanged = false;
        if (jsonGetFloat(msg, "heatThresh", &heatT)) {
            heatT = constrain(heatT, CN105_TEMP_MIN, CN105_TEMP_MAX);
            settings.get().heatingThreshold = heatT;
            threshChanged = true;
            LOG_INFO("[WebUI] Set heatingThreshold=%.1f", heatT);
        }
        if (jsonGetFloat(msg, "coolThresh", &coolT)) {
            coolT = constrain(coolT, CN105_TEMP_MIN, CN105_TEMP_MAX);
            settings.get().coolingThreshold = coolT;
            threshChanged = true;
            LOG_INFO("[WebUI] Set coolingThreshold=%.1f", coolT);
        }
        if (threshChanged) {
            // Enforce minimum 2°C gap (bidirectional)
            float h = settings.get().heatingThreshold;
            float c = settings.get().coolingThreshold;
            if (c - h < 2.0f) {
                // If cool was explicitly set, adjust heat downward
                if (jsonGetFloat(msg, "coolThresh", &coolT)) {
                    h = c - 2.0f;
                    if (h < CN105_TEMP_MIN) { h = CN105_TEMP_MIN; c = h + 2.0f; }
                } else {
                    // Heat was set, adjust cool upward
                    c = h + 2.0f;
                    if (c > CN105_TEMP_MAX) { c = CN105_TEMP_MAX; h = c - 2.0f; }
                }
                settings.get().heatingThreshold = h;
                settings.get().coolingThreshold = c;
            }
            settings.save();
        }

        // Immediately push state with wanted values so the client doesn't
        // have to wait up to 1s for the next periodic push (which may
        // carry stale values if it was already in-flight).
        pushState();

    } else if (strcmp(cmd, "config") == 0) {
        // ── Configuration commands ───────────────────────────────────────
        bool changed = false;
        int intVal;

        if (jsonGetInt(msg, "logLevel", &intVal)) {
            if (intVal >= LOG_LEVEL_ERROR && intVal <= LOG_LEVEL_DEBUG) {
                settings.get().logLevel = (LogLevel)intVal;
                currentLogLevel = (LogLevel)intVal;
                LOG_INFO("[WebUI] Config logLevel=%d", intVal);
                changed = true;
            }
        }

        if (jsonGetInt(msg, "pollInterval", &intVal)) {
            if (intVal >= 500 && intVal <= 30000) {
                settings.get().pollMs = (uint32_t)intVal;
                _ctrl->setUpdateInterval((uint32_t)intVal);
                LOG_INFO("[WebUI] Config pollInterval=%d", intVal);
                changed = true;
            }
        }

        if (jsonGetInt(msg, "vaneConfig", &intVal)) {
            if (intVal >= 0 && intVal <= 2) {
                settings.get().vaneConfig = (uint8_t)intVal;
                LOG_INFO("[WebUI] Config vaneConfig=%d", intVal);
                changed = true;
            }
        }

        char unitVal[4];
        if (jsonGetString(msg, "tempUnit", unitVal, sizeof(unitVal))) {
            bool useF = (strcmp(unitVal, "F") == 0);
            settings.get().useFahrenheit = useF;
            LOG_INFO("[WebUI] Config tempUnit=%s", useF ? "F" : "C");
            changed = true;
        }

        if (changed) {
            settings.save();
            // Push updated state to reflect new config values
            pushState();
        }

    } else if (strcmp(cmd, "wifi") == 0) {
        // ── WiFi credential update ──────────────────────────────────────
        const char *error = nullptr;
        if (!applyWifiCredentials(msg, &error)) {
            char errBuf[128];
            snprintf(errBuf, sizeof(errBuf), "{\"type\":\"error\",\"msg\":\"%s\"}", error);
            sendWsText(httpd_req_to_sockfd(req), errBuf);
            return;
        }
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"wifiSaved\"}");

    } else if (strcmp(cmd, "restart") == 0) {
        LOG_INFO("[WebUI] Restart requested");
        sendWsText(_wsClientFd, "{\"type\":\"info\",\"msg\":\"Restarting...\"}");
        delay(500);
        ESP.restart();

    } else if (strcmp(cmd, "hkReset") == 0) {
        LOG_WARN("[WebUI] HomeKit pairing reset requested");
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"info\",\"msg\":\"Removing HomeKit pairings...\"}");
        homeSpan.processSerialCommand("U");
        delay(500);
        ESP.restart();

    } else {
        LOG_WARN("[WebUI] Unknown command: %s", cmd);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Send text to WebSocket client
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::sendWsText(int fd, const char *text) {
    if (fd < 0 || !_server) return;

    // Verify the client is still a valid WebSocket connection
    httpd_ws_client_info_t info = httpd_ws_get_fd_info(_server, fd);
    if (info != HTTPD_WS_CLIENT_WEBSOCKET) {
        // Client disconnected or is no longer a WS client
        if (_wsClientFd == fd) {
            _wsClientFd = -1;
        }
        return;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)text;
    frame.len     = strlen(text);

    esp_err_t ret = httpd_ws_send_frame_async(_server, fd, &frame);
    if (ret != ESP_OK) {
        LOG_WARN("[WebUI] Failed to send WS frame to fd=%d: %d", fd, ret);
        if (_wsClientFd == fd) {
            _wsClientFd = -1;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Push full state JSON to connected WebSocket client
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::pushState() {
    if (_wsClientFd < 0) return;

    const CN105State st = _ctrl->getEffectiveState();
    const DeviceSettings &cfg = settings.get();

    char buf[800];
    int n = snprintf(buf, sizeof(buf),
        "{\"type\":\"state\""
        ",\"power\":%s"
        ",\"mode\":\"%s\""
        ",\"target\":%.1f"
        ",\"room\":%.1f"
        ",\"fan\":\"%s\""
        ",\"vane\":\"%s\""
        ",\"wideVane\":\"%s\""
        ",\"operating\":%s"
        ",\"compressorHz\":%u"
        ",\"connected\":%s"
        ",\"uptime\":%lu"
        ",\"rssi\":%d"
        ",\"wifiUptime\":%lu"
        ",\"subMode\":\"%s\""
        ",\"stage\":\"%s\""
        ",\"autoSubMode\":\"%s\"",
        st.power ? "true" : "false",
        modeToStr(st.mode),
        st.targetTemp,
        st.roomTemp,
        fanToStr(st.fanSpeed),
        vaneToStr(st.vane),
        wideVaneToStr(st.wideVane),
        st.operating ? "true" : "false",
        st.compressorHz,
        _ctrl->isConnected() ? "true" : "false",
        (unsigned long)(millis() / 1000),
        (int)WiFi.RSSI(),
        (unsigned long)wifiRecovery.getWifiUptimeSeconds(),
        webSubModeToStr(st.subMode),
        webStageToStr(st.stage),
        webAutoSubModeToStr(st.autoSubMode)
    );

    if (st.outsideTempValid) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"outsideTemp\":%.1f", st.outsideTemp);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"outsideTemp\":null");
    }

    // Error code
    if (st.hasError) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"errorCode\":%u", st.errorCode);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"errorCode\":null");
    }

    // Runtime hours
    if (st.runtimeValid) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"runtime\":%.1f", st.runtimeHours);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"runtime\":null");
    }

    // Dual setpoint thresholds
    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"heatThresh\":%.1f"
        ",\"coolThresh\":%.1f",
        cfg.heatingThreshold,
        cfg.coolingThreshold
    );

    // Count paired controllers
    int hkControllers = 0;
    for (auto it = homeSpan.controllerListBegin(); it != homeSpan.controllerListEnd(); ++it) {
        hkControllers++;
    }

    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"logLevel\":%d"
        ",\"pollInterval\":%lu"
        ",\"tempUnit\":\"%s\""
        ",\"vaneConfig\":%d"
        ",\"hkPaired\":%s"
        ",\"hkControllers\":%d"
        ",\"hkStatus\":\"%s\""
        ",\"hkSetupCode\":\"%s\""
        ",\"hkSetupURI\":\"%s\""
        "}",
        (int)cfg.logLevel,
        (unsigned long)cfg.pollMs,
        cfg.useFahrenheit ? "F" : "C",
        (int)cfg.vaneConfig,
        hkControllers > 0 ? "true" : "false",
        hkControllers,
        homeSpan.statusString(_hkStatus),
        _fmtCode,
        _setupURI
    );

    sendWsText(_wsClientFd, buf);
}

// ══════════════════════════════════════════════════════════════════════════════
// Broadcast a log message to connected WebSocket client
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::broadcastLog(const char *msg) {
    if (_wsClientFd < 0) return;

    char escaped[280];
    jsonEscape(msg, escaped, sizeof(escaped));

    char buf[320];
    snprintf(buf, sizeof(buf), "{\"type\":\"log\",\"msg\":\"%s\"}", escaped);
    sendWsText(_wsClientFd, buf);
}

// ══════════════════════════════════════════════════════════════════════════════
// loop() — called from main loop, pushes state every 1 second
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::loop() {
    if (_wsClientFd < 0) return;

    uint32_t now = millis();
    if (now - _lastStatePush >= 1000) {
        _lastStatePush = now;
        pushState();
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// setSetupCode() — store pairing code for web UI display
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::setSetupCode(const char *code) {
    if (code) {
        strncpy(_setupCode, code, sizeof(_setupCode) - 1);
        _setupCode[sizeof(_setupCode) - 1] = '\0';
    }
    updateCachedSetupInfo();
}

void WebUI::setQRID(const char *id) {
    if (id) {
        strncpy(_qrID, id, sizeof(_qrID) - 1);
        _qrID[sizeof(_qrID) - 1] = '\0';
    }
    updateCachedSetupInfo();
}

void WebUI::updateCachedSetupInfo() {
    if (strlen(_setupCode) < 8) return;

    // Format setup code as XXX-XX-XXX
    snprintf(_fmtCode, sizeof(_fmtCode), "%.3s-%.2s-%.3s",
             _setupCode, _setupCode + 3, _setupCode + 5);

    // Generate X-HM:// setup URI for QR code
    HapQR qr;
    const char *uri = qr.get(atoi(_setupCode), _qrID, 9);  // 9 = Thermostats
    strncpy(_setupURI, uri, sizeof(_setupURI) - 1);
    _setupURI[sizeof(_setupURI) - 1] = '\0';
}

// ══════════════════════════════════════════════════════════════════════════════
// begin() — start HTTP server on port 8080
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::begin(CN105Controller *ctrl) {
    _ctrl = ctrl;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = 8080;
    config.ctrl_port      = 32769;  // Different from default to avoid conflict
    config.stack_size     = 8192;   // Default 4096 too small for WS handlers + log buffers
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    LOG_INFO("[WebUI] Starting HTTP server on port %d", config.server_port);

    esp_err_t ret = httpd_start(&_server, &config);
    if (ret != ESP_OK) {
        LOG_ERROR("[WebUI] Failed to start HTTP server: %d", ret);
        return;
    }

    // Register GET / handler (serve HTML)
    const httpd_uri_t rootUri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = handleRoot,
        .user_ctx  = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    httpd_register_uri_handler(_server, &rootUri);

    // Register POST /upload handler (OTA firmware upload)
    const httpd_uri_t otaUri = {
        .uri       = "/upload",
        .method    = HTTP_POST,
        .handler   = handleOtaUpload,
        .user_ctx  = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    httpd_register_uri_handler(_server, &otaUri);

    // Register GET /ws handler (WebSocket)
    const httpd_uri_t wsUri = {
        .uri       = "/ws",
        .method    = HTTP_GET,
        .handler   = handleWebSocket,
        .user_ctx  = this,
        .is_websocket = true,
        .handle_ws_control_frames = true,
        .supported_subprotocol = NULL
    };
    httpd_register_uri_handler(_server, &wsUri);

    // Register GET /wifi-status handler
    const httpd_uri_t wifiStatusUri = {
        .uri       = "/wifi-status",
        .method    = HTTP_GET,
        .handler   = handleWifiStatus,
        .user_ctx  = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    httpd_register_uri_handler(_server, &wifiStatusUri);

    // Register POST /wifi-setup handler
    const httpd_uri_t wifiSetupUri = {
        .uri       = "/wifi-setup",
        .method    = HTTP_POST,
        .handler   = handleWifiSetup,
        .user_ctx  = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    httpd_register_uri_handler(_server, &wifiSetupUri);

    LOG_INFO("[WebUI] HTTP server started, WebSocket endpoint at /ws");

    // Register log hook to stream logs to WebSocket client
    logHook = [](const char *msg) { webUI.broadcastLog(msg); };

    // Track HomeSpan status changes for web UI
    homeSpan.setStatusCallback([](HS_STATUS status) {
        webUI._hkStatus = status;
    });
}
