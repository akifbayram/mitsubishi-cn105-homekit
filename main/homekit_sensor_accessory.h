#pragma once

#include "ble_config.h"

#ifdef BLE_ENABLE

/// Store the device serial used to derive the bridged sensor accessory's AID.
/// Call once during homekit_init, before homekit_sensor_begin().
void homekit_sensor_set_serial(const char* serial);

/// Create the Remote Sensor bridged accessory if a sensor is configured+enabled.
/// Call once during homekit_init, BEFORE hap_start() (adds it to the initial bridge DB).
void homekit_sensor_begin();

/// Main-loop entry: add/remove the sensor accessory when the BLE config changes,
/// and push temperature/humidity/battery to HomeKit (2s throttle internally).
void homekit_sensor_loop();

#endif // BLE_ENABLE
