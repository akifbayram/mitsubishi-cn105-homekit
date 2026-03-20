#include "web_server.h"
#include "cn105_strings.h"
#include "json_utils.h"
#include "wifi_manager.h"
#include "wifi_recovery.h"
#include "homekit_setup.h"
#include "esp_utils.h"
#include <algorithm>
#include <cmath>
#include "ble_config.h"
#ifdef BLE_ENABLE
#include "ble_sensor.h"
#endif

static const char *TAG = "web_ws";

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket handler: GET /ws
// ══════════════════════════════════════════════════════════════════════════════

esp_err_t WebUI::handleWebSocket(httpd_req_t *req) {
    // On first call (handshake), req->method == HTTP_GET
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        int oldFd = webUI._wsClientFd.exchange(fd);
        if (oldFd >= 0 && oldFd != fd) {
            LOG_INFO("Replacing WS client fd=%d with fd=%d", oldFd, fd);
        } else {
            LOG_INFO("WebSocket client connected (fd=%d)", fd);
        }
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
        return ret;
    }

    if (frame.len == 0) {
        return ESP_OK;  // Control frames handled by ESP-IDF (handle_ws_control_frames=false)
    }

    // Allocate buffer and receive payload
    if (frame.len > 1024) {
        LOG_WARN("WS frame too large (%d bytes), ignoring", (int)frame.len);
        return ESP_OK;
    }

    uint8_t *buf = (uint8_t *)malloc(frame.len + 1);
    if (!buf) {
        LOG_ERROR("Failed to allocate WS receive buffer");
        return ESP_ERR_NO_MEM;
    }

    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    // Null-terminate for string processing
    buf[frame.len] = '\0';

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        LOG_DEBUG("WS received: %s", (char *)buf);
        webUI.handleWsMessage(req, (const char *)buf);
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
        LOG_WARN("WS message missing 'cmd' field");
        return;
    }

    if (strcmp(cmd, "set") == 0) {
        // ── Control commands ─────────────────────────────────────────────
        bool hasControlChange = false;

        bool boolVal;
        if (jsonGetBool(msg, "power", &boolVal)) {
            LOG_INFO("Set power=%s", boolVal ? "ON" : "OFF");
            _ctrl->setPower(boolVal);
            hasControlChange = true;
        }

        char strVal[16];
        if (jsonGetString(msg, "mode", strVal, sizeof(strVal))) {
            uint8_t mode = strToMode(strVal);
            LOG_INFO("Set mode=%s (0x%02X)", strVal, mode);
            _ctrl->setMode(mode);
            hasControlChange = true;
        }

        float floatVal;
        if (jsonGetFloat(msg, "target", &floatVal)) {
            LOG_INFO("Set target=%.1f", floatVal);
            _ctrl->setTargetTemp(floatVal);
            hasControlChange = true;
        }

        if (jsonGetString(msg, "fan", strVal, sizeof(strVal))) {
            uint8_t fan = strToFan(strVal);
            LOG_INFO("Set fan=%s (0x%02X)", strVal, fan);
            _ctrl->setFanSpeed(fan);
            hasControlChange = true;
        }

        if (jsonGetString(msg, "vane", strVal, sizeof(strVal))) {
            uint8_t vane = strToVane(strVal);
            LOG_INFO("Set vane=%s (0x%02X)", strVal, vane);
            _ctrl->setVane(vane);
            hasControlChange = true;
        }

        if (jsonGetString(msg, "wideVane", strVal, sizeof(strVal))) {
            uint8_t wv = strToWideVane(strVal);
            LOG_INFO("Set wideVane=%s (0x%02X)", strVal, wv);
            _ctrl->setWideVane(wv);
            hasControlChange = true;
        }

        if (hasControlChange) {
            _ctrl->sendPendingChanges();
        }

        float heatT, coolT;
        bool heatSet = false, coolSet = false;
        if (jsonGetFloat(msg, "heatThresh", &heatT)) {
            heatT = std::clamp(heatT, CN105_TEMP_MIN, CN105_TEMP_MAX);
            settings.get().heatingThreshold = heatT;
            heatSet = true;
            LOG_INFO("Set heatingThreshold=%.1f", heatT);
        }
        if (jsonGetFloat(msg, "coolThresh", &coolT)) {
            coolT = std::clamp(coolT, CN105_TEMP_MIN, CN105_TEMP_MAX);
            settings.get().coolingThreshold = coolT;
            coolSet = true;
            LOG_INFO("Set coolingThreshold=%.1f", coolT);
        }
        if (heatSet || coolSet) {
            // Enforce minimum 2 deg C gap (bidirectional)
            float h = settings.get().heatingThreshold;
            float c = settings.get().coolingThreshold;
            if (c - h < 2.0f) {
                if (coolSet) {
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
                logging_set_level((LogLevel)intVal);
                LOG_INFO("Config logLevel=%d", intVal);
                changed = true;
            }
        }

        if (jsonGetInt(msg, "pollInterval", &intVal)) {
            if (intVal >= 500 && intVal <= 30000) {
                settings.get().pollMs = (uint32_t)intVal;
                _ctrl->setUpdateInterval((uint32_t)intVal);
                LOG_INFO("Config pollInterval=%d", intVal);
                changed = true;
            }
        }

        if (jsonGetInt(msg, "vaneConfig", &intVal)) {
            if (intVal >= 0 && intVal <= 2) {
                settings.get().vaneConfig = (uint8_t)intVal;
                LOG_INFO("Config vaneConfig=%d", intVal);
                changed = true;
            }
        }

        char unitVal[4];
        if (jsonGetString(msg, "tempUnit", unitVal, sizeof(unitVal))) {
            bool useF = (strcmp(unitVal, "F") == 0);
            settings.get().useFahrenheit = useF;
            LOG_INFO("Config tempUnit=%s", useF ? "F" : "C");
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
            // Empty after trim -> reset to default
            if (strlen(start) == 0) {
                start = (char *)BRAND_NAME;
            }
            strncpy(settings.get().deviceName, start, sizeof(settings.get().deviceName) - 1);
            settings.get().deviceName[sizeof(settings.get().deviceName) - 1] = '\0';
            LOG_INFO("Config deviceName=%s", settings.get().deviceName);
            changed = true;
        }

#ifdef BLE_ENABLE
        bool bleEnabledVal;
        if (jsonGetBool(msg, "bleEnabled", &bleEnabledVal)) {
            BleSensor::setBleEnabled(bleEnabledVal);
            LOG_INFO("Config bleEnabled=%s", bleEnabledVal ? "ON" : "OFF");
            pushState();
        }

        char bleAddrVal[18];
        if (jsonGetString(msg, "bleAddr", bleAddrVal, sizeof(bleAddrVal))) {
            BleSensor::setAddr(bleAddrVal);
            LOG_INFO("Config bleAddr=%s", bleAddrVal);
        }

        bool bleFeedVal;
        if (jsonGetBool(msg, "bleFeed", &bleFeedVal)) {
            BleSensor::setEnabled(bleFeedVal);
            LOG_INFO("Config bleFeed=%s", bleFeedVal ? "ON" : "OFF");
        }

        int bleTimeoutVal;
        if (jsonGetInt(msg, "bleTimeout", &bleTimeoutVal)) {
            if (bleTimeoutVal >= 30 && bleTimeoutVal <= 600) {
                settings.get().bleStaleTimeoutS = (uint16_t)bleTimeoutVal;
                settings.save();
                LOG_INFO("Config bleTimeout=%ds", bleTimeoutVal);
            }
        }
#endif

        if (changed) {
            settings.save();
            // Push updated state to reflect new config values
            pushState();
        }

    } else if (strcmp(cmd, "bleScan") == 0) {
#ifdef BLE_ENABLE
        if (!BleSensor::isBleEnabled()) {
            LOG_WARN("BLE scan rejected — BLE not enabled");
        } else if (!BleSensor::isDiscovering()) {
            BleSensor::startDiscovery();
            LOG_INFO("BLE discovery scan requested");
        }
#endif

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
        LOG_INFO("Restart requested");
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"info\",\"msg\":\"Restarting...\"}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();

    } else if (strcmp(cmd, "hkReset") == 0) {
        LOG_WARN("HomeKit pairing reset requested");
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"info\",\"msg\":\"Removing HomeKit pairings...\"}");
        homekit_reset_pairings();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();

    } else {
        LOG_WARN("Unknown command: %s", cmd);
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
        // Client disconnected or is no longer a WS client — atomic CAS
        // avoids clearing a newly-connected client's fd in a race
        int expected = fd;
        _wsClientFd.compare_exchange_strong(expected, -1);
        return;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)text;
    frame.len     = strlen(text);

    esp_err_t ret = httpd_ws_send_frame_async(_server, fd, &frame);
    if (ret != ESP_OK) {
        // Reset fd BEFORE logging — prevents broadcastLog from retrying the dead socket.
        // Atomic CAS: only clear if fd hasn't been replaced by a new client.
        int expected = fd;
        _wsClientFd.compare_exchange_strong(expected, -1);
        LOG_WARN("Failed to send WS frame to fd=%d: %d, client cleared", fd, ret);
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

    unsigned long wifiUptimeSec = wifiRecovery.getWifiUptimeSeconds();

    char buf[1200];
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
        (unsigned long)(uptime_ms() / 1000),
        (int)WifiManager::getRSSI(),
        wifiUptimeSec,
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

    int hkControllers = homekit_get_controller_count();

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
        homekit_get_status_string(),
        homekit_get_setup_code(),
        homekit_get_setup_payload()
    );

#ifdef BLE_ENABLE
    {
        float bleT = BleSensor::temperature();
        float bleH = BleSensor::humidity();
        int8_t bleB = BleSensor::battery();
        uint32_t staleMs = BleSensor::lastUpdateAge();
        if (staleMs == UINT32_MAX) staleMs = 0;

        char bleTStr[8] = "null", bleHStr[8] = "null";
        if (!std::isnan(bleT)) snprintf(bleTStr, sizeof(bleTStr), "%.1f", bleT);
        if (!std::isnan(bleH)) snprintf(bleHStr, sizeof(bleHStr), "%.0f", bleH);

        const char* sType = BleSensor::sensorType();

        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"bleEnabled\":%s"
            ",\"bleTemp\":%s"
            ",\"bleHumidity\":%s"
            ",\"bleBattery\":%d"
            ",\"bleRssi\":%d"
            ",\"bleActive\":%s"
            ",\"bleStale\":%s"
            ",\"bleStaleMs\":%lu"
            ",\"bleAddr\":\"%s\""
            ",\"bleFeed\":%s"
            ",\"bleTimeout\":%u"
            ",\"bleDiscovering\":%s"
            ",\"bleSensorType\":%s%s%s",
            BleSensor::isBleEnabled() ? "true" : "false",
            bleTStr,
            bleHStr,
            (int)bleB,
            BleSensor::rssi(),
            BleSensor::isActive() ? "true" : "false",
            BleSensor::isStale() ? "true" : "false",
            (unsigned long)staleMs,
            BleSensor::getAddr(),
            BleSensor::isEnabled() ? "true" : "false",
            (unsigned int)settings.get().bleStaleTimeoutS,
            BleSensor::isDiscovering() ? "true" : "false",
            sType ? "\"" : "", sType ? sType : "null", sType ? "\"" : ""
        );
    }
#endif

    n += snprintf(buf + n, sizeof(buf) - n, "}");

    if (n >= (int)sizeof(buf)) {
        LOG_WARN("pushState buffer truncated (%d >= %zu), skipping send", n, sizeof(buf));
        return;
    }

    sendWsText(_wsClientFd, buf);
}

// ══════════════════════════════════════════════════════════════════════════════
// Broadcast a log message to connected WebSocket client
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::broadcastLog(const char *msg, size_t len) {
    if (_wsClientFd < 0) return;

    // Static buffers — safe because this is only called from the vprintf hook
    // which runs under the ESP-IDF log lock (one task at a time).  Avoids
    // ~600 bytes of stack pressure on constrained tasks like the WiFi task.
    static char escaped[280];
    jsonEscape(msg, escaped, sizeof(escaped));

    static char buf[320];
    snprintf(buf, sizeof(buf), "{\"type\":\"log\",\"msg\":\"%s\"}", escaped);
    sendWsText(_wsClientFd, buf);
}

// ══════════════════════════════════════════════════════════════════════════════
// BLE discovery results push
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::pushDiscoveryResults(bool done) {
#ifdef BLE_ENABLE
    if (_wsClientFd < 0) return;

    int count = 0;
    const BleDiscoveredDevice* devs = BleSensor::discoveryResults(count);

    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"bleScanResults\",\"done\":%s,\"devices\":[",
                     done ? "true" : "false");

    for (int i = 0; i < count; i++) {
        if (n >= (int)sizeof(buf) - 100) break;  // Reserve space for entry + closing
        char escName[50];
        jsonEscape(devs[i].name, escName, sizeof(escName));
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"addr\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"rssi\":%d}",
            i > 0 ? "," : "",
            devs[i].addr, escName, devs[i].type, devs[i].rssi);
    }

    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    sendWsText(_wsClientFd, buf);
#endif
}

// ══════════════════════════════════════════════════════════════════════════════
// loop() — called from main loop, pushes state every 1 second
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::loop() {
    if (_wsClientFd < 0) return;

    uint32_t now = uptime_ms();
    if (now - _lastStatePush >= 1000) {
        _lastStatePush = now;
        pushState();
    }

    // Server-side WS ping every 15s to detect dead clients (half-open TCP)
    if (now - _lastWsPing >= 15000) {
        _lastWsPing = now;
        int fd = _wsClientFd.load();
        if (fd < 0) return;
        httpd_ws_client_info_t info = httpd_ws_get_fd_info(_server, fd);
        if (info != HTTPD_WS_CLIENT_WEBSOCKET) {
            LOG_INFO("Dead WS client detected (fd=%d), cleaning up", fd);
            int expected = fd;
            _wsClientFd.compare_exchange_strong(expected, -1);
            return;
        }
        httpd_ws_frame_t ping;
        memset(&ping, 0, sizeof(ping));
        ping.type = HTTPD_WS_TYPE_PING;
        esp_err_t ret = httpd_ws_send_frame_async(_server, fd, &ping);
        if (ret != ESP_OK) {
            LOG_WARN("Ping failed (fd=%d): %d, cleaning up", fd, ret);
            int expected = fd;
            _wsClientFd.compare_exchange_strong(expected, -1);
            return;
        }
    }

#ifdef BLE_ENABLE
    if (BleSensor::pollDiscoveryComplete()) {
        pushDiscoveryResults(true);
    } else if (BleSensor::pollDiscoveryUpdate()) {
        pushDiscoveryResults(false);
    }
#endif
}
