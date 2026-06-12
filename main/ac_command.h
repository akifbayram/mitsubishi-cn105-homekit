#pragma once

#include <cstdlib>
#include <cstring>
#include "cn105_protocol.h"
#include "cn105_strings.h"

// Shared control-command application — used by the WebSocket "set" handler
// and the MQTT command topics. Applies exactly one field as a pending change;
// the caller is responsible for ctrl.sendPendingChanges() afterwards.
//
// field: "power" | "mode" | "temperature" | "fan" | "vane" | "widevane"
// value: lowercase web-string form (see cn105_strings.h); power accepts
//        "on"/"off"/"true"/"false"/"1"/"0" (case-insensitive)
// Returns true if the field was recognized and applied.
inline bool acApplyCommand(CN105Controller &ctrl, const char *field, const char *value) {
    if (strcmp(field, "power") == 0) {
        bool on = (strcasecmp(value, "on") == 0 || strcasecmp(value, "true") == 0 ||
                   strcmp(value, "1") == 0);
        ctrl.setPower(on);
        return true;
    }
    if (strcmp(field, "mode") == 0) {
        ctrl.setMode(strToMode(value));
        return true;
    }
    if (strcmp(field, "temperature") == 0) {
        char *end;
        float t = strtof(value, &end);
        if (end == value) return false;
        ctrl.setTargetTemp(t);  // clamps to CN105_TEMP_MIN..MAX internally
        return true;
    }
    if (strcmp(field, "fan") == 0) {
        ctrl.setFanSpeed(strToFan(value));
        return true;
    }
    if (strcmp(field, "vane") == 0) {
        ctrl.setVane(strToVane(value));
        return true;
    }
    if (strcmp(field, "widevane") == 0) {
        ctrl.setWideVane(strToWideVane(value));
        return true;
    }
    return false;
}
