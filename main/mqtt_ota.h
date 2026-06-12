#pragma once

#include "mqtt_config.h"

#ifdef MQTT_ENABLE

namespace MqttOta {
    // Start a firmware download+install from url, verifying against sha256
    // (64 hex chars, mandatory). Returns false if another OTA is in progress
    // or the task could not be created. Progress is published to
    // <base>/ota/status by the download task.
    bool start(const char *url, const char *sha256);
}

#endif // MQTT_ENABLE
