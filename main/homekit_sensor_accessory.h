#pragma once

#include "ble_config.h"

#ifdef BLE_ENABLE

/// Create the Remote Sensor bridged accessory if a sensor MAC is configured.
/// Call once during homekit_init, BEFORE hap_start() (adds it to the initial bridge DB).
/// `serial` (used to derive the accessory AID + a unique "-rs" SerialNumber) and
/// `fwRev` are stored for later lazy creation if a MAC is first configured at runtime.
void homekit_sensor_begin(const char* serial, const char* fwRev);

/// Main-loop entry: lazily creates the sensor accessory once a MAC is configured
/// (never removed afterwards), and pushes temperature/humidity/battery plus
/// StatusActive to HomeKit (2s throttle internally). StatusActive goes false —
/// surfacing as "Not Responding" — when BLE is disabled or data is stale.
void homekit_sensor_loop();

#endif // BLE_ENABLE
