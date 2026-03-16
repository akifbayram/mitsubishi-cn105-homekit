#include "web_server.h"
#include "cn105_strings.h"
#include "json_utils.h"
#include "wifi_recovery.h"
#include <WiFi.h>
#include "HomeSpan.h"
#include "ble_sensor.h"

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

        char nameVal[32];
        if (jsonGetString(msg, "deviceName", nameVal, sizeof(nameVal))) {
            // Trim leading whitespace
            char *start = nameVal;
            while (*start == ' ') start++;
            // Trim trailing whitespace
            char *end = start + strlen(start) - 1;
            while (end > start && *end == ' ') *end-- = '\0';
            // Empty after trim → reset to default
            if (strlen(start) == 0) {
                start = (char *)BRAND_NAME;
            }
            strncpy(settings.get().deviceName, start, sizeof(settings.get().deviceName) - 1);
            settings.get().deviceName[sizeof(settings.get().deviceName) - 1] = '\0';
            LOG_INFO("[WebUI] Config deviceName=%s", settings.get().deviceName);
            changed = true;
        }

#ifdef BLE_ENABLE
        char bleAddrVal[18];
        if (jsonGetString(msg, "bleAddr", bleAddrVal, sizeof(bleAddrVal))) {
            BleSensor::setAddr(bleAddrVal);
            LOG_INFO("[WebUI] Config bleAddr=%s", bleAddrVal);
        }

        bool bleFeedVal;
        if (jsonGetBool(msg, "bleFeed", &bleFeedVal)) {
            BleSensor::setEnabled(bleFeedVal);
            LOG_INFO("[WebUI] Config bleFeed=%s", bleFeedVal ? "ON" : "OFF");
        }

        int bleTimeoutVal;
        if (jsonGetInt(msg, "bleTimeout", &bleTimeoutVal)) {
            if (bleTimeoutVal >= 30 && bleTimeoutVal <= 600) {
                settings.get().bleStaleTimeoutS = (uint16_t)bleTimeoutVal;
                settings.save();
                LOG_INFO("[WebUI] Config bleTimeout=%ds", bleTimeoutVal);
            }
        }
#endif

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

    char escName[65];
    jsonEscape(cfg.deviceName, escName, sizeof(escName));

    char buf[1152];
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
        ",\"autoSubMode\":\"%s\""
        ",\"deviceName\":\"%s\"",
        st.power ? "true" : "false",
        modeToWebStr(st.mode),
        st.targetTemp,
        st.roomTemp,
        fanToWebStr(st.fanSpeed),
        vaneToWebStr(st.vane),
        wideVaneToWebStr(st.wideVane),
        st.operating ? "true" : "false",
        st.compressorHz,
        _ctrl->isConnected() ? "true" : "false",
        (unsigned long)(millis() / 1000),
        (int)WiFi.RSSI(),
        (unsigned long)wifiRecovery.getWifiUptimeSeconds(),
        subModeToWebStr(st.subMode),
        stageToWebStr(st.stage),
        autoSubModeToWebStr(st.autoSubMode),
        escName
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
        ",\"hkSetupURI\":\"%s\"",
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

#ifdef BLE_ENABLE
    {
        float bleT = BleSensor::temperature();
        float bleH = BleSensor::humidity();
        int8_t bleB = BleSensor::battery();
        uint32_t staleMs = BleSensor::lastUpdateAge();
        if (staleMs == UINT32_MAX) staleMs = 0;

        char bleTStr[8] = "null", bleHStr[8] = "null";
        if (!isnan(bleT)) snprintf(bleTStr, sizeof(bleTStr), "%.1f", bleT);
        if (!isnan(bleH)) snprintf(bleHStr, sizeof(bleHStr), "%.0f", bleH);

        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"bleTemp\":%s"
            ",\"bleHumidity\":%s"
            ",\"bleBattery\":%d"
            ",\"bleRssi\":%d"
            ",\"bleActive\":%s"
            ",\"bleStale\":%s"
            ",\"bleStaleMs\":%lu"
            ",\"bleAddr\":\"%s\""
            ",\"bleFeed\":%s"
            ",\"bleTimeout\":%u",
            bleTStr,
            bleHStr,
            (int)bleB,
            BleSensor::rssi(),
            BleSensor::isActive() ? "true" : "false",
            BleSensor::isStale() ? "true" : "false",
            (unsigned long)staleMs,
            BleSensor::getAddr(),
            BleSensor::isEnabled() ? "true" : "false",
            (unsigned int)settings.get().bleStaleTimeoutS
        );
    }
#endif

    n += snprintf(buf + n, sizeof(buf) - n, "}");

    if (n >= (int)sizeof(buf)) {
        LOG_WARN("[WebUI] pushState buffer truncated (%d >= %zu)", n, sizeof(buf));
    }

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
