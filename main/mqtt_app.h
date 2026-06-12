#pragma once

#include "mqtt_config.h"

#ifdef MQTT_ENABLE

#include "cn105_protocol.h"

// Named mqtt_app (not mqtt_client) because ESP-IDF's esp-mqtt component owns
// the mqtt_client.h header name.
namespace MqttClient {
    void begin(CN105Controller *ctrl);  // Derives topics from MAC; does NOT connect
    void loop();                        // Call ~1 Hz from main loop; lazy connect/disconnect
    void applyConfig();                 // Settings changed via web UI — reconnect with new config
    bool isConnected();
    const char *statusStr();            // "off" | "connecting" | "connected" | "error"
    const char *baseTopic();            // Effective base topic (configured or derived)

    // Publish a JSON payload to <base>/ota/status (used by mqtt_ota.cpp)
    void publishOtaStatus(const char *json);
}

#endif // MQTT_ENABLE
