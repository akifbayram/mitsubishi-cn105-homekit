#include "web_server.h"
#include "cn105_strings.h"
#include "json_utils.h"
#include "event_log.h"
#include "time_sync.h"
#include "wifi_manager.h"
#include "wifi_recovery.h"
#include "homekit_setup.h"
#include "esp_utils.h"
#include "board_profile.h"
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <nvs_flash.h>
#include <lwip/sockets.h>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <cmath>
#include "ble_config.h"
#ifdef BLE_ENABLE
#include "ble_sensor.h"
#endif
#include "espnow_link.h"
#include "link_sensor.h"
#include "room_avg.h"

static const char *TAG = "web_ws";

// Free-heap floor for the WS log stream (bytes). Sustained DEBUG streaming to
// a connected client consumed ~45 KB/min and the WiFi driver dies silently
// (no disconnect event) once free heap reaches ~40 KB — observed 2026-07-12 on
// AtomS3 Lite. Shed log frames well above that line; resume with hysteresis so
// the stream doesn't flap at the boundary. State pushes (1 Hz, ~1.4 KB) are
// deliberately NOT gated — they keep the UI alive and are too small to matter.
static constexpr uint32_t WS_LOG_SHED_HEAP   = 60 * 1024;
static constexpr uint32_t WS_LOG_RESUME_HEAP = 70 * 1024;


// ══════════════════════════════════════════════════════════════════════════════
// WebSocket handler: GET /ws
// ══════════════════════════════════════════════════════════════════════════════

esp_err_t WebUI::handleWebSocket(httpd_req_t *req) {
    // On first call (handshake), req->method == HTTP_GET
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);

        // Cap send() blocking time so the httpd task can't hang for minutes when
        // a client disappears without closing the WebSocket (half-open TCP).
        // After timeout the send fails, ESP-IDF closes the session, and the
        // httpd task returns to its select() loop within 5 s.
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        LOG_INFO("WebSocket client connected (fd=%d)", fd);
        // Push initial state immediately (broadcast reaches this new client too)
        webUI.pushState();
        webUI.sendDeviceInfo(fd);
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

// The availability rule now lives in RoomAvg (room_avg.h) so the legacy
// roomSource command and the dial's own source edit apply the same test.
static inline bool roomMemberAvailable(int bit) { return RoomAvg::memberAvailable(bit); }

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
            if (!mode_mask_allows(settings.get().modeMask, mode)) {
                // Stale/foreign client — the UI hides disabled modes.
                LOG_WARN("Set mode=%s rejected — disabled by capability mask", strVal);
            } else {
                LOG_INFO("Set mode=%s (0x%02X)", strVal, mode);
                _ctrl->setMode(mode);
                hasControlChange = true;
            }
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

        if (jsonGetInt(msg, "modeMask", &intVal)) {
            uint8_t m = mode_mask_sanitize((uint8_t)intVal);
            settings.get().modeMask = m;
            LOG_INFO("Config modeMask=0x%02X", m);
            changed = true;
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
            pushState();  // setAddr() already persists; just confirm to the UI at once
        }
#endif

        // roomStaleTimeoutS governs BOTH remote feeds (BLE + Link), so like
        // roomSource it's handled outside the BLE ifdef. Key name is legacy.
        int bleTimeoutVal;
        if (jsonGetInt(msg, "bleTimeout", &bleTimeoutVal)) {
            if (bleTimeoutVal >= 30 && bleTimeoutVal <= 3600) {
                settings.get().roomStaleTimeoutS = (uint16_t)bleTimeoutVal;
                LOG_INFO("Config bleTimeout=%ds", bleTimeoutVal);
                changed = true;  // saved + pushState() handled by the `changed` block below
            }
        }

        // roomSource is not BLE-specific (Internal/Link don't need BLE at
        // all) so it's validated and saved unconditionally, outside the
        // #ifdef above. Same acceptance test as h_room_sensor()
        // (espnow_link.cpp) so the dial and the web UI can't disagree —
        // RoomAvg::legacySrcSelectable() is that one test. It rejects
        // out-of-range values, Link with no sensing dial, and BLE with no
        // configured slot; any of those would silently leave the pump on its
        // internal sensor while the UI claimed otherwise.
        int roomSourceVal;
        if (jsonGetInt(msg, "roomSource", &roomSourceVal)) {
            if (roomSourceVal >= 0 && roomSourceVal <= UINT8_MAX &&
                RoomAvg::legacySrcSelectable((uint8_t)roomSourceVal)) {
                // Legacy enum command (kept for a cached pre-averaging UI):
                // an explicit pick is a single-mode selection.
                settings.get().roomMode   = 0;
                settings.get().roomSingle = room_single_from_legacy((uint8_t)roomSourceVal);
                settings.save();
                LOG_INFO("Config roomSource=%d", roomSourceVal);
            }
            // Push on accept AND reject: the web UI paints its source rows
            // optimistically on tap, and this echo is what confirms the
            // selection or snaps a rejected one back.
            pushState();
        }

        // ── Blending model (averaging rework). Writes are echoed with ONE
        //    pushState at the end, on accept AND reject, same contract as
        //    roomSource: the UI paints optimistically and self-corrects.
        //    Saves coalesce the same way — one NVS commit per message. ──
        bool roomSave = false;   // an accepted room-model write needs a save
        bool roomPush = false;   // any room/BLE key was seen (ack even rejects)

        int roomModeVal;
        if (jsonGetInt(msg, "roomMode", &roomModeVal)) {
            roomPush = true;
            if (roomModeVal == 0 || roomModeVal == 1) {
                settings.get().roomMode = (uint8_t)roomModeVal;
                roomSave = true;
                LOG_INFO("Config roomMode=%d", roomModeVal);
            }
        }

        int roomSingleVal;
        if (jsonGetInt(msg, "roomSingle", &roomSingleVal)) {
            roomPush = true;
            if (roomSingleVal >= 0 && roomSingleVal < ROOM_MEMBER_COUNT &&
                roomMemberAvailable(roomSingleVal)) {
                settings.get().roomSingle = (uint8_t)roomSingleVal;
                roomSave = true;
                LOG_INFO("Config roomSingle=%d", roomSingleVal);
            }
        }

        int roomMembersVal;
        if (jsonGetInt(msg, "roomMembers", &roomMembersVal)) {
            roomPush = true;
            uint8_t m = (uint8_t)roomMembersVal & (uint8_t)((1u << ROOM_MEMBER_COUNT) - 1);
            // Strip bits with nothing behind them so a stray client can't
            // check a ghost member. Internal is stripped too: it is never a
            // blend member (see room_avg.cpp) and the UI offers no checkbox.
            for (int bit = 0; bit < ROOM_MEMBER_COUNT; bit++) {
                if (bit == ROOM_MEMBER_INTERNAL || !roomMemberAvailable(bit))
                    m &= (uint8_t)~(1u << bit);
            }
            settings.get().roomMembers = m;
            roomSave = true;
            LOG_INFO("Config roomMembers=0x%02X", m);
        }

        int roomOffIdx;
        if (jsonGetInt(msg, "roomOffIdx", &roomOffIdx)) {
            roomPush = true;
            int v = 0;
            if (jsonGetInt(msg, "roomOffVal", &v) &&
                roomOffIdx >= 0 && roomOffIdx < ROOM_MEMBER_COUNT) {
                v = std::clamp(v, (int)-ROOM_OFFSET_MAX_TENTHS, (int)ROOM_OFFSET_MAX_TENTHS);
                settings.get().roomOffsets[roomOffIdx] = (int8_t)v;
                roomSave = true;
                LOG_INFO("Config roomOff[%d]=%d", roomOffIdx, v);
            }
        }

#ifdef BLE_ENABLE
        // The sensor-list commands save inside BleSensor (setSensor owns the
        // readings/scan side effects) — they only need the ack push here.
        char addMac[18];
        if (jsonGetString(msg, "bleAddMac", addMac, sizeof(addMac))) {
            roomPush = true;
            // Re-adding a known MAC updates that sensor; otherwise take the
            // first free slot. Full list -> tell the client, nothing changes.
            int slot = -1;
            for (int i = ROOM_MAX_BLE_SENSORS - 1; i >= 0; i--) {
                if (!settings.get().bleSensors[i].addr[0]) slot = i;
                if (strcasecmp(settings.get().bleSensors[i].addr, addMac) == 0) { slot = i; break; }
            }
            if (slot < 0) {
                sendWsText(httpd_req_to_sockfd(req),
                           "{\"type\":\"error\",\"msg\":\"Sensor list is full\"}");
            } else {
                char addName[24] = "";
                jsonGetString(msg, "bleAddName", addName, sizeof(addName));
                char defName[24];
                snprintf(defName, sizeof(defName), "Sensor %d", slot + 1);
                bool keepStored = settings.get().bleSensors[slot].name[0] != '\0';
                BleSensor::setSensor(slot, addMac,
                                     addName[0] ? addName : (keepStored ? nullptr : defName));
                LOG_INFO("Config bleAdd slot=%d mac=%s", slot, addMac);
            }
        }

        int delIdx;
        if (jsonGetInt(msg, "bleDelIdx", &delIdx)) {
            roomPush = true;
            BleSensor::setSensor(delIdx, "", nullptr);
            LOG_INFO("Config bleDel slot=%d", delIdx);
        }

        int renIdx;
        if (jsonGetInt(msg, "bleRenIdx", &renIdx)) {
            roomPush = true;
            char renName[24];
            if (jsonGetString(msg, "bleRenName", renName, sizeof(renName)) && renName[0])
                BleSensor::renameSensor(renIdx, renName);
        }
#endif

        if (roomSave) settings.save();
        if (changed) {
            settings.save();
            // Push updated state to reflect new config values
            pushState();
        } else if (roomPush) {
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

    } else if (strcmp(cmd, "diag") == 0) {
        // ── Device event history (About card / diagnostics copy) ────────
        // 24 events × ~75 B + counters — too big for a stack frame here.
        constexpr size_t DIAG_BUF = 2816;
        char *out = (char *)malloc(DIAG_BUF);
        if (!out) return;
        int n = snprintf(out, DIAG_BUF, "{\"type\":\"events\",");
        int m = eventlog_json(out + n, DIAG_BUF - n);
        if (m > 0 && n + m + 1 < (int)DIAG_BUF) {
            n += m;
            out[n++] = '}';
            out[n] = '\0';
            sendWsText(httpd_req_to_sockfd(req), out);
        }
        free(out);

    } else if (strcmp(cmd, "factoryReset") == 0) {
        // ── Full factory reset — wipes EVERYTHING in NVS ─────────────────
        // Settings, WiFi credentials, HomeKit pairings + setup code, dial
        // bonds, event history: all namespaces live in the default "nvs"
        // partition. Guarded by an explicit confirm string so a stray or
        // malformed frame can't wipe a unit.
        char confirmVal[8] = {0};
        jsonGetString(msg, "confirm", confirmVal, sizeof(confirmVal));
        if (strcmp(confirmVal, "ERASE") != 0) {
            sendWsText(httpd_req_to_sockfd(req),
                       "{\"type\":\"error\",\"msg\":\"Factory reset not confirmed\"}");
            return;
        }
        LOG_WARN("FACTORY RESET — erasing all NVS and restarting");
        sendWsText(httpd_req_to_sockfd(req),
                   "{\"type\":\"info\",\"msg\":\"Factory reset — erasing all settings and restarting...\"}");
        vTaskDelay(pdMS_TO_TICKS(300));   // let the frame flush
        nvs_flash_deinit();               // close handles; stray writes now fail cleanly
        nvs_flash_erase();
        esp_restart();

    } else if (strcmp(cmd, "restart") == 0) {
        LOG_INFO("Restart requested");
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"info\",\"msg\":\"Restarting...\"}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();

    } else if (strcmp(cmd, "hkReset") == 0) {
        LOG_WARN("HomeKit pairing reset requested");
        eventlog_append(EV_HK_RESET);
        if (homekit_reset_pairings()) {
            // Success path reboots shortly after (SDK behavior)
            sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"info\",\"msg\":\"Removing HomeKit pairings...\"}");
        } else {
            sendWsText(httpd_req_to_sockfd(req),
                       "{\"type\":\"error\",\"msg\":\"HomeKit is not running - nothing to reset\"}");
        }

    } else if (strcmp(cmd, "wifiForget") == 0) {
        // ── Erase stored WiFi credentials, reboot into the setup AP ──────
        // Deliberately NOT gated by a typed confirm string. factoryReset is,
        // because it is irreversible; this is recoverable in under a minute
        // from the AP, so it sits with hkReset / forgetRemote / restart.
        LOG_WARN("WiFi credential erase requested");
        eventlog_append(EV_WIFI_CREDS_CHANGED, 1);   // code 1 = erased
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"wifiForgotten\"}");
        vTaskDelay(pdMS_TO_TICKS(300));   // let the frame flush before we go
        WifiManager::eraseCredentials();
        esp_restart();

    } else if (strcmp(cmd, "forgetRemote") == 0) {
        LOG_WARN("ESP-NOW remote forget requested");
        sendWsText(httpd_req_to_sockfd(req), "{\"type\":\"info\",\"msg\":\"Forgetting remote...\"}");
        espnow_forget_and_restart();

    } else if (strcmp(cmd, "pairRemote") == 0) {
        LOG_INFO("ESP-NOW pairing window requested");
        espnowLink.startPairing();
        sendWsText(httpd_req_to_sockfd(req),
                   "{\"type\":\"info\",\"msg\":\"Listening for a remote...\"}");

    } else if (strcmp(cmd, "pairCancel") == 0) {
        LOG_INFO("ESP-NOW pairing cancelled");
        espnowLink.cancelPairing();
        sendWsText(httpd_req_to_sockfd(req),
                   "{\"type\":\"info\",\"msg\":\"Pairing cancelled\"}");

    } else {
        LOG_WARN("Unknown command: %s", cmd);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Send text to WebSocket client
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::sendWsText(int fd, const char *text) {
    if (fd < 0 || !_server) return;

    // Serialize all frame writes: httpd_ws_send_frame_async() sends on the
    // CALLER'S task, and senders live on many tasks (log hook on whatever task
    // logged, state push on main + httpd). Two unserialized sends interleave
    // mid-frame on the wire and the browser kills the socket (1007 invalid
    // data). Bounded take: a wedged client can hold a send for the full 5s
    // SO_SNDTIMEO — drop the frame rather than stall logging tasks behind it
    // (logs are best-effort; state re-pushes at 1 Hz).
    if (!_wsSendMux || xSemaphoreTake(_wsSendMux, pdMS_TO_TICKS(100)) != pdTRUE) return;

    esp_err_t ret = ESP_OK;
    // Skip fds that are no longer live WebSocket sessions (closed / plain HTTP).
    if (httpd_ws_get_fd_info(_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.type    = HTTPD_WS_TYPE_TEXT;
        frame.payload = (uint8_t *)text;
        frame.len     = strlen(text);
        ret = httpd_ws_send_frame_async(_server, fd, &frame);
    }
    xSemaphoreGive(_wsSendMux);

    if (ret != ESP_OK) {
        // The warn is queued to the log ring (no re-entry into this send
        // path); httpd reaps the dead socket on its own.
        LOG_WARN("WS send to fd=%d failed: %d", fd, ret);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket client enumeration + broadcast
// ══════════════════════════════════════════════════════════════════════════════

// Fill `out` with the fds of all live WebSocket clients. Returns the count.
int WebUI::collectWsClients(int *out, int maxOut) {
    if (!_server) return 0;
    size_t cnt = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(_server, &cnt, fds) != ESP_OK) return 0;
    int n = 0;
    for (size_t i = 0; i < cnt && n < maxOut; i++) {
        if (httpd_ws_get_fd_info(_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            out[n++] = fds[i];
        }
    }
    return n;
}

// Send `text` to every connected WebSocket client. Each socket's LRU counter is
// refreshed first: a server-push-only WebSocket never *receives* traffic, so
// esp_http_server's LRU logic would otherwise purge it when the socket pool
// fills (a browser opening parallel HTTP connections, or a second viewer).
void WebUI::broadcastWs(const char *text) {
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    int n = collectWsClients(fds, CONFIG_LWIP_MAX_SOCKETS);
    for (int i = 0; i < n; i++) {
        httpd_sess_update_lru_counter(_server, fds[i]);
        sendWsText(fds[i], text);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Push full state JSON to all connected WebSocket clients
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::pushState() {
    int wsFds[CONFIG_LWIP_MAX_SOCKETS];
    int wsN = collectWsClients(wsFds, CONFIG_LWIP_MAX_SOCKETS);
    if (wsN == 0) return;  // nobody listening — skip building the JSON

    const CN105State st = _ctrl->getEffectiveState();
    const DeviceSettings &cfg = settings.get();

    char escName[65];
    jsonEscape(cfg.deviceName, escName, sizeof(escName));

    unsigned long wifiUptimeSec = wifiRecovery.getWifiUptimeSeconds();

    /* Cached copy (refreshed on credential change/reconnect) — getSSID() is an
     * esp_wifi_get_config() struct copy, too heavy for a 1 Hz push. */
    char ssid[33] = "";
    wifiRecovery.getCachedSSID(ssid, sizeof(ssid));
    char escSsid[65];
    jsonEscape(ssid, escSsid, sizeof(escSsid));

    // Heap, not stack: the averaging rework (per-sensor list + blend status)
    // outgrew what pushState may burn on the httpd task's stack.
    constexpr size_t bufSz = 3584;
    char *buf = (char *)malloc(bufSz);
    if (!buf) return;
    int n = snprintf(buf, bufSz,
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
        ",\"deviceName\":\"%s\""
        ",\"ssid\":\"%s\"",
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
        escName,
        escSsid
    );

    if (st.outsideTempValid) {
        jsonAppend(buf, bufSz, &n, ",\"outsideTemp\":%.1f", st.outsideTemp);
    } else {
        jsonAppend(buf, bufSz, &n, ",\"outsideTemp\":null");
    }

    // Error code
    if (st.hasError) {
        jsonAppend(buf, bufSz, &n, ",\"errorCode\":%u", st.errorCode);
    } else {
        jsonAppend(buf, bufSz, &n, ",\"errorCode\":null");
    }

    // Runtime hours
    if (st.runtimeValid) {
        jsonAppend(buf, bufSz, &n, ",\"runtime\":%.1f", st.runtimeHours);
    } else {
        jsonAppend(buf, bufSz, &n, ",\"runtime\":null");
    }

    // Heap + reboot/connectivity health. resetReason/crashCount make a silent
    // field reboot classifiable over the network (the boot banner scrolls out
    // of the 12-line log ring before a client can reconnect); wifiDrops/
    // cn105Drops/epoch add connectivity health (epoch 0 = wall clock not
    // SNTP-synced yet).
    jsonAppend(buf, bufSz, &n,
        ",\"heapFree\":%lu"
        ",\"heapMin\":%lu"
        ",\"heapBlock\":%lu"
        ",\"resetReason\":\"%s\""
        ",\"crashCount\":%lu"
        ",\"wifiDrops\":%lu"
        ",\"cn105Drops\":%lu"
        ",\"epoch\":%lu",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        resetReasonStr(esp_reset_reason()),
        (unsigned long)appCrashCount(),
        (unsigned long)WifiManager::getDisconnectCount(),
        (unsigned long)eventlog_session_count(EV_CN105_LOST),
        (unsigned long)time_sync_epoch()
    );

    // Dual setpoint thresholds
    jsonAppend(buf, bufSz, &n,
        ",\"heatThresh\":%.1f"
        ",\"coolThresh\":%.1f",
        cfg.heatingThreshold,
        cfg.coolingThreshold
    );

    // ── ESP-NOW remote (Dial) status ──────────────────────────────────────
    {
        uint8_t rm[6]; espnowLink.getPeerMac(rm);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 rm[0],rm[1],rm[2],rm[3],rm[4],rm[5]);
        jsonAppend(buf, bufSz, &n,
            ",\"remoteBonded\":%s,\"remoteLive\":%s,\"remoteMac\":\"%s\""
            ",\"remotePairing\":%s,\"remotePairSecs\":%d,\"remotePairResult\":\"%s\"",
            espnowLink.isBonded() ? "true" : "false",
            espnowLink.isPeerLive() ? "true" : "false",
            espnowLink.isBonded() ? macStr : "",
            espnowLink.pairingActive() ? "true" : "false",
            espnowLink.pairingSecondsLeft(),
            espnowLink.pairResult());

        EspnowDialDetail dd;
        if (espnowLink.getDialDetail(dd)) {
            // remoteCertState: sl2_cert_state_t — 0 NONE (unprovisioned dial,
            // the common home-built case) / 1 PRESENT / 2 INVALID / 3 OK.
            jsonAppend(buf, bufSz, &n,
                ",\"remoteLastSeen\":%ld,\"remoteSyncing\":%s,\"remoteCertState\":%u",
                (long)dd.lastSeenSec, dd.syncing ? "true" : "false",
                (unsigned)dd.certState);
            if (dd.rssi != 0)
                jsonAppend(buf, bufSz, &n, ",\"remoteRssi\":%d", dd.rssi);
            if (dd.haveInfo && dd.model[0]) {
                char escModel[64], escFw[48];
                jsonEscape(dd.model, escModel, sizeof(escModel));
                jsonEscape(dd.fw, escFw, sizeof(escFw));
                jsonAppend(buf, bufSz, &n,
                    ",\"remoteModel\":\"%s\",\"remoteFw\":\"%s\"", escModel, escFw);
            }
        }
    }

    // Before HomeKit has started, its fields are placeholders — neutralize
    // them here so clients don't need started-ness special cases: boot mask
    // mirrors the current mask (nothing pending a restart), setup code/URI
    // are empty. hkReady itself still drives the panel's placeholder state.
    bool hkReady = homekit_is_started();
    int hkControllers = homekit_get_controller_count();

    jsonAppend(buf, bufSz, &n,
        ",\"logLevel\":%d"
        ",\"pollInterval\":%lu"
        ",\"tempUnit\":\"%s\""
        ",\"vaneConfig\":%d"
        ",\"modeMask\":%d"
        ",\"modeMaskBoot\":%d"
        ",\"hkReady\":%s"
        ",\"hkPaired\":%s"
        ",\"hkControllers\":%d"
        ",\"hkStatus\":\"%s\""
        ",\"hkSetupCode\":\"%s\""
        ",\"hkSetupURI\":\"%s\"",
        (int)cfg.logLevel,
        (unsigned long)cfg.pollMs,
        cfg.useFahrenheit ? "F" : "C",
        (int)cfg.vaneConfig,
        (int)cfg.modeMask,
        (int)(hkReady ? homekit_get_boot_mode_mask() : cfg.modeMask),
        hkReady ? "true" : "false",
        (hkReady && hkControllers > 0) ? "true" : "false",
        hkControllers,
        homekit_get_status_string(),
        hkReady ? homekit_get_setup_code() : "",
        hkReady ? homekit_get_setup_payload() : ""
    );

    {
        // Room source + Serin Link sensor — deliberately OUTSIDE the
        // BLE_ENABLE block: Internal/Link need no BLE, and a BLE-disabled
        // build must still render the Room Sensor card (Heat Pump + Serin
        // Link rows). roomSource/roomStatus are the same values Task 14's
        // INFO TLV sends the dial, so web and dial can't disagree.
        // "bleTimeout" keeps its legacy key — it now governs both feeds,
        // but renaming would break exported settings files.
        float linkT = LinkSensor::temperature();
        float linkH = LinkSensor::humidity();
        uint32_t linkAge = LinkSensor::lastUpdateAge();
        if (linkAge == UINT32_MAX) linkAge = 0;
        char linkTStr[8] = "null", linkHStr[8] = "null";
        if (!std::isnan(linkT)) snprintf(linkTStr, sizeof(linkTStr), "%.2f", linkT);
        if (!std::isnan(linkH)) snprintf(linkHStr, sizeof(linkHStr), "%.0f", linkH);

        jsonAppend(buf, bufSz, &n,
            ",\"roomSource\":%d"
            ",\"roomStatus\":%d"
            ",\"bleTimeout\":%u"
            ",\"hasLinkSensor\":%s"
            ",\"linkTemp\":%s"
            ",\"linkHumidity\":%s"
            ",\"linkActive\":%s"
            ",\"linkStale\":%s"
            ",\"linkStaleMs\":%lu",
            (int)settings.get().roomSource,
            (int)espnowLink.roomSourceStatus(),
            (unsigned int)settings.get().roomStaleTimeoutS,
            LinkSensor::hasSensor() ? "true" : "false",
            linkTStr,
            linkHStr,
            LinkSensor::isActive() ? "true" : "false",
            LinkSensor::isStale() ? "true" : "false",
            (unsigned long)linkAge);
    }

    {
        // Blending model + last blend pass. Everything the card needs to
        // render modes/exclusions/banners rides here — no client inference.
        const RoomAvg::Status av = RoomAvg::status();
        char effStr[8] = "null";
        if (!std::isnan(av.effective)) snprintf(effStr, sizeof(effStr), "%.2f", av.effective);
        jsonAppend(buf, bufSz, &n,
            ",\"roomMode\":%u"
            ",\"roomSingle\":%u"
            ",\"roomMembers\":%u"
            ",\"avgFeeding\":%s"
            ",\"avgFallback\":%s"
            ",\"effTemp\":%s"
            ",\"spread\":%.2f"
            ",\"effAge\":%lu"
            ",\"contrib\":%u"
            ",\"exclStale\":%u"
            ",\"exclOff\":%u",
            (unsigned)cfg.roomMode,
            (unsigned)cfg.roomSingle,
            (unsigned)cfg.roomMembers,
            av.feeding ? "true" : "false",
            av.fallback ? "true" : "false",
            effStr,
            av.spread,
            (unsigned long)(av.effAgeMs == UINT32_MAX ? 0 : av.effAgeMs),
            (unsigned)av.contributors,
            (unsigned)av.exclStale,
            (unsigned)av.exclOff);
        jsonAppend(buf, bufSz, &n, ",\"roomOffs\":[");
        for (int i = 0; i < ROOM_MEMBER_COUNT; i++)
            jsonAppend(buf, bufSz, &n, "%s%d", i ? "," : "", (int)cfg.roomOffsets[i]);
        jsonAppend(buf, bufSz, &n, "]");
    }

#ifdef BLE_ENABLE
    {
        // Legacy flat slot-0 fields, duplicated by bleSensors[0] below. Kept
        // deliberately: a cached pre-averaging index.html renders from these
        // after an OTA (same compat window as the roomSource command), and
        // the diagnostics copy embeds bleAddr. Drop once that window closes.
        float bleT = BleSensor::temperature();
        float bleH = BleSensor::humidity();
        int8_t bleB = BleSensor::battery();
        uint32_t staleMs = BleSensor::lastUpdateAge();
        if (staleMs == UINT32_MAX) staleMs = 0;

        char bleTStr[8] = "null", bleHStr[8] = "null";
        if (!std::isnan(bleT)) snprintf(bleTStr, sizeof(bleTStr), "%.2f", bleT);
        if (!std::isnan(bleH)) snprintf(bleHStr, sizeof(bleHStr), "%.0f", bleH);

        const char* sType = BleSensor::sensorType();

        jsonAppend(buf, bufSz, &n,
            ",\"bleEnabled\":%s"
            ",\"bleTemp\":%s"
            ",\"bleHumidity\":%s"
            ",\"bleBattery\":%d"
            ",\"bleRssi\":%d"
            ",\"bleActive\":%s"
            ",\"bleStale\":%s"
            ",\"bleReverted\":%s"
            ",\"bleStaleMs\":%lu"
            ",\"bleAddr\":\"%s\""
            ",\"bleBattLow\":%s"
            ",\"bleDiscovering\":%s"
            ",\"bleSensorType\":%s%s%s",
            BleSensor::isBleEnabled() ? "true" : "false",
            bleTStr,
            bleHStr,
            (int)bleB,
            BleSensor::rssi(),
            BleSensor::isActive() ? "true" : "false",
            BleSensor::isStale() ? "true" : "false",
            BleSensor::isReverted() ? "true" : "false",
            (unsigned long)staleMs,
            BleSensor::getAddr(),
            (bleB >= 0 && bleB <= BLE_BATT_LOW_PCT) ? "true" : "false",
            BleSensor::isDiscovering() ? "true" : "false",
            sType ? "\"" : "", sType ? sType : "null", sType ? "\"" : ""
        );
    }

    {
        // Named sensor list — one entry per configured slot, slot index
        // included so member bits and offsets line up client-side.
        jsonAppend(buf, bufSz, &n, ",\"bleSensors\":[");
        bool first = true;
        for (int i = 0; i < ROOM_MAX_BLE_SENSORS; i++) {
            if (!BleSensor::isConfigured(i)) continue;
            float t = BleSensor::temperature(i);
            float h = BleSensor::humidity(i);
            uint32_t age = BleSensor::lastUpdateAge(i);
            char tS[8] = "null", hS[8] = "null";
            if (!std::isnan(t)) snprintf(tS, sizeof(tS), "%.2f", t);
            if (!std::isnan(h)) snprintf(hS, sizeof(hS), "%.0f", h);
            char escSensName[50];
            jsonEscape(settings.get().bleSensors[i].name, escSensName, sizeof(escSensName));
            const char* ty = BleSensor::sensorType(i);
            jsonAppend(buf, bufSz, &n,
                "%s{\"i\":%d,\"addr\":\"%s\",\"name\":\"%s\",\"type\":%s%s%s"
                ",\"temp\":%s,\"hum\":%s,\"batt\":%d,\"rssi\":%d,\"age\":%lu"
                ",\"active\":%s,\"stale\":%s}",
                first ? "" : ",", i,
                settings.get().bleSensors[i].addr, escSensName,
                ty ? "\"" : "", ty ? ty : "null", ty ? "\"" : "",
                tS, hS, (int)BleSensor::battery(i), BleSensor::rssi(i),
                (unsigned long)(age == UINT32_MAX ? 0 : age),
                BleSensor::isActive(i) ? "true" : "false",
                BleSensor::isStale(i) ? "true" : "false");
            first = false;
        }
        jsonAppend(buf, bufSz, &n, "]");
    }
#endif

    jsonAppend(buf, bufSz, &n, "}");

    if (n >= (int)bufSz) {
        LOG_WARN("pushState buffer truncated (%d >= %zu), skipping send", n, bufSz);
        free(buf);
        return;
    }

    for (int i = 0; i < wsN; i++) {
        httpd_sess_update_lru_counter(_server, wsFds[i]);
        sendWsText(wsFds[i], buf);
    }
    free(buf);
}

// ══════════════════════════════════════════════════════════════════════════════
// Broadcast a log message to connected WebSocket client
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::broadcastLog(const char *msg, size_t len) {
    if (!_server) return;

    // Heap-floor shed (thresholds + rationale above). CONFIG_LOG_VERSION_1
    // calls the vprintf hook UNLOCKED, so concurrent loggers can race this
    // gate: the atomic keeps the flag un-torn, and the worst case is one
    // frame shed or sent late — the threshold check self-corrects on the
    // next log line.
    static std::atomic<bool> shedding{false};
    uint32_t freeHeap = esp_get_free_heap_size();
    if (!shedding && freeHeap < WS_LOG_SHED_HEAP) {
        shedding = true;
        // Queued into the log ring like any line and drained next tick; safe
        // because it fires only on the off->on shed transition, and while
        // shedding is on the drained copy is dropped right here — no loop.
        LOG_WARN("WS log stream paused: heapFree=%lu below %u",
                 (unsigned long)freeHeap, (unsigned)WS_LOG_SHED_HEAP);
    } else if (shedding && freeHeap > WS_LOG_RESUME_HEAP) {
        shedding = false;
    }
    if (shedding) return;

    // Static buffers — safe by design: broadcastLog's only caller is the
    // main-task drain in WebUI::loop() (single consumer of the log ring),
    // so no two invocations can overlap. Static keeps ~600 B off the stack.
    static char escaped[280];
    jsonEscape(msg, escaped, sizeof(escaped));

    static char buf[320];
    snprintf(buf, sizeof(buf), "{\"type\":\"log\",\"msg\":\"%s\"}", escaped);
    broadcastWs(buf);
}

// ══════════════════════════════════════════════════════════════════════════════
// Device info frame — one-shot on WebSocket connect
// ══════════════════════════════════════════════════════════════════════════════

// Everything here is constant for the life of the connection (or changes only
// with a reboot), so it rides a single frame at connect instead of bloating
// the 1 Hz state push. The UI keeps it for the About card + diagnostics copy.
void WebUI::sendDeviceInfo(int fd) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ip[16] = "";
    WifiManager::getIP(ip, sizeof(ip));
    const esp_app_desc_t *app = esp_app_get_description();

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"deviceInfo\""
        ",\"board\":\"%s\""
        ",\"fw\":\"%s\""
        ",\"idf\":\"%s\""
        ",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\""
        ",\"ip\":\"%s\""
        ",\"hostname\":\"%s\""
        ",\"resetReason\":\"%s\""
        ",\"bootCount\":%lu"
        ",\"crashTotal\":%lu"
        ",\"safeMode\":%s}",
        BOARD_NAME,
        app->version,
        esp_get_idf_version(),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        ip,
        WifiManager::getHostname(),
        reset_reason_str(esp_reset_reason()),
        (unsigned long)eventlog_boot_count(),
        (unsigned long)eventlog_crash_total(),
        eventlog_safe_mode() ? "true" : "false");
    sendWsText(fd, buf);
}

// ══════════════════════════════════════════════════════════════════════════════
// BLE discovery results push
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::pushDiscoveryResults(bool done) {
#ifdef BLE_ENABLE
    if (!_server) return;

    BleDiscoveredDevice devs[BLE_MAX_DISCOVERED];
    bool truncated = false;
    int count = BleSensor::discoveryResults(devs, BLE_MAX_DISCOVERED, &truncated);

    // Worst-case JSON per device (addr + escaped name + type + fields); also the
    // loop's reserve bound, so the buffer always fits BLE_MAX_DISCOVERED entries
    constexpr size_t ENTRY_JSON_MAX = 160;
    char buf[128 /* header + closing */ + BLE_MAX_DISCOVERED * ENTRY_JSON_MAX];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"bleScanResults\",\"done\":%s,\"truncated\":%s,\"devices\":[",
                     done ? "true" : "false", truncated ? "true" : "false");

    for (int i = 0; i < count; i++) {
        if (n >= (int)(sizeof(buf) - ENTRY_JSON_MAX)) break;  // Reserve space for entry + closing
        char escName[50];
        jsonEscape(devs[i].name, escName, sizeof(escName));
        char tStr[8] = "null", hStr[8] = "null";
        if (!std::isnan(devs[i].temperature)) snprintf(tStr, sizeof(tStr), "%.1f", devs[i].temperature);
        if (!std::isnan(devs[i].humidity))    snprintf(hStr, sizeof(hStr), "%.0f", devs[i].humidity);
        jsonAppend(buf, sizeof(buf), &n,
            "%s{\"addr\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"rssi\":%d,\"temp\":%s,\"hum\":%s}",
            i > 0 ? "," : "",
            devs[i].addr, escName, devs[i].type, devs[i].rssi, tStr, hStr);
    }

    jsonAppend(buf, sizeof(buf), &n, "]}");

    if (n >= (int)sizeof(buf)) {
        LOG_WARN("pushDiscoveryResults buffer truncated (%d >= %zu), skipping send", n, sizeof(buf));
        return;
    }

    broadcastWs(buf);
#endif
}

// ══════════════════════════════════════════════════════════════════════════════
// loop() — called from main loop, pushes state every 1 second
// ══════════════════════════════════════════════════════════════════════════════

void WebUI::loop() {
    if (!_server) return;

    uint32_t now = uptime_ms();
    if (now - _lastStatePush >= 1000) {
        _lastStatePush = now;
        pushState();
    }
    // Liveness: the 1s push exercises each socket; a dead/half-open socket fails
    // on send (SO_SNDTIMEO) and esp_http_server reaps it, so the next
    // collectWsClients() simply omits it. No separate ping loop needed.

    // Drain queued log lines to WS clients — bounded per 10 ms tick so one
    // slow client send (SO_SNDTIMEO allows up to 5 s) can't monopolize the
    // main loop. Producers never touch sockets (threading contract in
    // logging.cpp); this is the single consumer.
    char logLine[256];
    for (int i = 0; i < 4; i++) {
        size_t len = logging_drain(logLine, sizeof(logLine));
        if (len == 0) break;
        broadcastLog(logLine, len);
    }

#ifdef BLE_ENABLE
    if (BleSensor::pollDiscoveryComplete()) {
        pushDiscoveryResults(true);
    } else if (BleSensor::pollDiscoveryUpdate()) {
        pushDiscoveryResults(false);
    }
#endif
}
