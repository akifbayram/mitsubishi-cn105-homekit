#include "mqtt_app.h"

#ifdef MQTT_ENABLE

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_mac.h>
#include <mqtt_client.h>   // ESP-IDF esp-mqtt public API

#include "logging.h"
#include "settings.h"
#include "esp_utils.h"
#include "wifi_manager.h"
#include "branding.h"
#include "json_utils.h"
#include "ac_command.h"
#include "mqtt_ota.h"
#include "ota_writer.h"
#include "ble_config.h"
#ifdef BLE_ENABLE
#include "ble_sensor.h"
#endif

static const char *TAG = "mqtt";

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

// ── Module state ────────────────────────────────────────────────────────────
static CN105Controller *s_ctrl = nullptr;
static esp_mqtt_client_handle_t s_client = nullptr;
static std::atomic<bool> s_connected{false};
static std::atomic<bool> s_forcePublish{false};
static std::atomic<bool> s_reconnectPending{false};
static bool s_started = false;
static const char *s_status = "off";

static char s_base[48];        // effective base topic
static char s_topicAvail[64];
static char s_topicState[64];
static char s_topicOtaStatus[72];
static char s_nodeId[24];      // HA discovery node id, e.g. "cn105_ab12"
static char s_serial[13];      // full MAC hex
static char s_deviceJson[256]; // shared HA discovery device block

static uint32_t s_lastPublishCheck = 0;
static uint32_t s_lastHeartbeat = 0;

// Implemented by later tasks (state publish / HA discovery / command dispatch):
static void publishStateIfChanged(bool force);
static void publishDiscovery();
static void handleData(esp_mqtt_event_handle_t event);

// ── Event handler ───────────────────────────────────────────────────────────
static void mqttEventHandler(void *arg, esp_event_base_t base,
                             int32_t eventId, void *eventData) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;
    switch ((esp_mqtt_event_id_t)eventId) {
    case MQTT_EVENT_CONNECTED: {
        LOG_INFO("Connected to broker");
        s_connected = true;
        s_status = "connected";
        esp_mqtt_client_publish(s_client, s_topicAvail, "online", 0, 1, 1);
        char t[80];
        static const char *cmdFields[] = {"power", "mode", "temperature",
                                          "fan", "vane", "widevane"};
        for (const char *f : cmdFields) {
            snprintf(t, sizeof(t), "%s/%s/set", s_base, f);
            esp_mqtt_client_subscribe(s_client, t, 0);
        }
        snprintf(t, sizeof(t), "%s/ota/install", s_base);
        esp_mqtt_client_subscribe(s_client, t, 0);
        publishDiscovery();
        s_forcePublish = true;
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        LOG_WARN("Disconnected from broker");
        s_connected = false;
        s_status = "connecting";
        break;
    case MQTT_EVENT_ERROR:
        if (!s_connected) s_status = "error";
        break;
    case MQTT_EVENT_DATA:
        handleData(event);
        break;
    default:
        break;
    }
}

// ── Lifecycle ───────────────────────────────────────────────────────────────
static void startClient() {
    const DeviceSettings &cfg = settings.get();

    // Effective base topic: user-configured, else derived from MAC (set in begin())
    if (strlen(cfg.mqttBaseTopic) > 0) {
        strncpy(s_base, cfg.mqttBaseTopic, sizeof(s_base) - 1);
        s_base[sizeof(s_base) - 1] = '\0';
    }
    snprintf(s_topicAvail, sizeof(s_topicAvail), "%s/availability", s_base);
    snprintf(s_topicState, sizeof(s_topicState), "%s/state", s_base);
    snprintf(s_topicOtaStatus, sizeof(s_topicOtaStatus), "%s/ota/status", s_base);

    // esp-mqtt strdup's all config strings (uri, user, pass, LWT) during
    // esp_mqtt_client_init(), so these buffers need only outlive that call.
    static char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", cfg.mqttHost, cfg.mqttPort);

    esp_mqtt_client_config_t mc = {};
    mc.broker.address.uri = uri;
    if (strlen(cfg.mqttUser) > 0) {
        mc.credentials.username = cfg.mqttUser;
        mc.credentials.authentication.password = cfg.mqttPass;
    }
    mc.session.last_will.topic = s_topicAvail;
    mc.session.last_will.msg = "offline";
    mc.session.last_will.msg_len = 0;  // 0 = use strlen
    mc.session.last_will.qos = 1;
    mc.session.last_will.retain = 1;
    mc.session.keepalive = 30;

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        LOG_ERROR("esp_mqtt_client_init failed");
        s_status = "error";
        return;
    }
    esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqttEventHandler, nullptr);
    esp_mqtt_client_start(s_client);
    s_started = true;
    s_status = "connecting";
    LOG_INFO("MQTT client started (broker %s, base topic %s)", uri, s_base);
}

static void stopClient() {
    if (!s_client) return;
    // Best-effort offline (LWT covers unclean exits)
    if (s_connected) {
        esp_mqtt_client_publish(s_client, s_topicAvail, "offline", 0, 1, 1);
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = nullptr;
    s_started = false;
    s_connected = false;
    s_status = "off";
    LOG_INFO("MQTT client stopped");
}

// ── Public API ──────────────────────────────────────────────────────────────
void MqttClient::begin(CN105Controller *ctrl) {
    s_ctrl = ctrl;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_nodeId, sizeof(s_nodeId), "cn105_%02x%02x", mac[4], mac[5]);
    snprintf(s_base, sizeof(s_base), "mitsubishi/%02X%02X", mac[4], mac[5]);

    char escName[65];
    jsonEscape(settings.get().deviceName, escName, sizeof(escName));
    snprintf(s_deviceJson, sizeof(s_deviceJson),
        "{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"%s\",\"mdl\":\"%s\",\"sw\":\"%s\"}",
        s_serial, escName, BRAND_MANUFACTURER, BRAND_MODEL, FW_VERSION);
}

void MqttClient::loop() {
    // Deferred reconnect (requested by applyConfig from another task): tear
    // down here in the main task, never while an OTA may be publishing status.
    if (s_reconnectPending && !OtaWriter::isInProgress()) {
        s_reconnectPending = false;
        if (s_started) stopClient();  // reconnects below if still enabled
    }

    const DeviceSettings &cfg = settings.get();
    bool want = cfg.mqttEnabled && strlen(cfg.mqttHost) > 0 && WifiManager::isConnected();

    if (want && !s_started) startClient();
    else if (!want && s_started && !OtaWriter::isInProgress()) stopClient();

    if (!s_connected) return;

    uint32_t now = uptime_ms();
    if (s_forcePublish || now - s_lastPublishCheck >= 1000) {
        s_lastPublishCheck = now;
        bool heartbeat = (now - s_lastHeartbeat >= 60000);
        bool force = s_forcePublish.exchange(false) || heartbeat;
        publishStateIfChanged(force);
    }
}

void MqttClient::applyConfig() {
    // Defer teardown to loop() (main task): avoids blocking the caller's task
    // on esp_mqtt_client_stop() and never destroys the client while an OTA
    // task may be publishing status on it.
    s_reconnectPending = true;
}

bool MqttClient::isConnected() { return s_connected; }
const char *MqttClient::statusStr() { return s_status; }
const char *MqttClient::baseTopic() { return s_base; }

void MqttClient::publishOtaStatus(const char *json) {
    if (!s_client || !s_connected) return;
    esp_mqtt_client_publish(s_client, s_topicOtaStatus, json, 0, 1, 0);
}

// ── State JSON ──────────────────────────────────────────────────────────────

// HA climate "action" values: off, heating, cooling, drying, fan, idle.
static const char *haAction(const CN105State &st) {
    if (!st.power) return "off";
    uint8_t m = st.mode;
    if (m == CN105_MODE_AUTO) {
        // Resolve AUTO to its active side from 0x09 polling
        if (st.autoSubMode == 0x02)      m = CN105_MODE_HEAT;
        else if (st.autoSubMode == 0x01) m = CN105_MODE_COOL;
    }
    if (m == CN105_MODE_FAN) return "fan";
    if (m == CN105_MODE_DRY) return st.operating ? "drying" : "idle";
    if (!st.operating) return "idle";
    if (m == CN105_MODE_HEAT) return "heating";
    if (m == CN105_MODE_COOL) return "cooling";
    // AUTO with autoSubMode 0x00 (OFF) or 0x03 (LEADER): active side unknown → idle
    return "idle";
}

static int buildStateJson(char *buf, size_t cap) {
    // getEffectiveState() substitutes wanted values during the anti-flicker
    // grace window, so HA doesn't bounce after a command.
    const CN105State st = s_ctrl->getEffectiveState();

    // haMode: HA climate mode vocabulary ("off" when powered down, "fan_only")
    const char *haMode = !st.power ? "off"
        : (st.mode == CN105_MODE_FAN ? "fan_only" : modeToWebStr(st.mode));

    int n = snprintf(buf, cap,
        "{\"power\":%s"
        ",\"mode\":\"%s\""
        ",\"haMode\":\"%s\""
        ",\"action\":\"%s\""
        ",\"target\":%.1f"
        ",\"room\":%.1f"
        ",\"fan\":\"%s\""
        ",\"vane\":\"%s\""
        ",\"wideVane\":\"%s\""
        ",\"operating\":%s"
        ",\"compressorHz\":%u"
        ",\"connected\":%s"
        ",\"subMode\":\"%s\""
        ",\"stage\":\"%s\""
        ",\"autoSubMode\":\"%s\"",
        st.power ? "true" : "false",
        modeToWebStr(st.mode),
        haMode,
        haAction(st),
        st.targetTemp,
        st.roomTemp,
        fanToWebStr(st.fanSpeed),
        vaneToWebStr(st.vane),
        wideVaneToWebStr(st.wideVane),
        st.operating ? "true" : "false",
        st.compressorHz,
        s_ctrl->isConnected() ? "true" : "false",
        subModeToWebStr(st.subMode),
        stageToWebStr(st.stage),
        autoSubModeToWebStr(st.autoSubMode));

    if (st.outsideTempValid)
        jsonAppend(buf, cap, &n, ",\"outsideTemp\":%.1f", st.outsideTemp);
    else
        jsonAppend(buf, cap, &n, ",\"outsideTemp\":null");

    if (st.hasError)
        jsonAppend(buf, cap, &n, ",\"errorCode\":%u", st.errorCode);
    else
        jsonAppend(buf, cap, &n, ",\"errorCode\":null");

    if (st.runtimeValid)
        jsonAppend(buf, cap, &n, ",\"runtime\":%.1f", st.runtimeHours);
    else
        jsonAppend(buf, cap, &n, ",\"runtime\":null");

    jsonAppend(buf, cap, &n,
        ",\"heatThresh\":%.1f,\"coolThresh\":%.1f",
        settings.get().heatingThreshold, settings.get().coolingThreshold);

#ifdef BLE_ENABLE
    if (BleSensor::isBleEnabled()) {
        float bleT = BleSensor::temperature();
        float bleH = BleSensor::humidity();
        char bleTStr[8] = "null", bleHStr[8] = "null";
        if (!std::isnan(bleT)) snprintf(bleTStr, sizeof(bleTStr), "%.1f", bleT);
        if (!std::isnan(bleH)) snprintf(bleHStr, sizeof(bleHStr), "%.0f", bleH);
        jsonAppend(buf, cap, &n,
            ",\"bleTemp\":%s,\"bleHumidity\":%s,\"bleBattery\":%d",
            bleTStr, bleHStr, (int)BleSensor::battery());
    }
#endif

    jsonAppend(buf, cap, &n, "}");
    return n;
}

// ── Placeholder bodies (filled in by later tasks) ───────────────────────────
static void publishStateIfChanged(bool force) {
    // Called from the main loop only (1 Hz) — static buffers are safe.
    static char json[1024];
    static char lastJson[1024] = "";

    int n = buildStateJson(json, sizeof(json));
    if (n >= (int)sizeof(json)) {
        LOG_WARN("MQTT state JSON truncated (%d >= %zu), skipping publish", n, sizeof(json));
        return;
    }

    if (!force && strcmp(json, lastJson) == 0) return;

    // Retained so HA gets fresh state immediately on its own restart.
    int rc = esp_mqtt_client_publish(s_client, s_topicState, json, 0, 0, 1);
    if (rc >= 0) {
        strcpy(lastJson, json);
        s_lastHeartbeat = uptime_ms();
        LOG_DEBUG("State published (%d bytes)", n);
    }
}
static void publishDiscovery() {}
static void handleData(esp_mqtt_event_handle_t event) { (void)event; }

#endif // MQTT_ENABLE
