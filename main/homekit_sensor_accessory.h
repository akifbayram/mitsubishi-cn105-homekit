#pragma once

#include "ble_config.h"

#ifdef BLE_ENABLE

/// Create the Remote Sensor bridged accessory if a sensor is configured+enabled.
/// Call once during homekit_init, BEFORE hap_start() (adds it to the initial bridge DB).
/// `serial` (used to derive the accessory AID) and `fwRev` are stored for later
/// lazy (re)creation when the BLE config changes at runtime.
void homekit_sensor_begin(const char* serial, const char* fwRev);

/// Main-loop entry: add/remove the sensor accessory when the BLE config changes,
/// and push temperature/humidity/battery to HomeKit (2s throttle internally).
void homekit_sensor_loop();

#endif // BLE_ENABLE
