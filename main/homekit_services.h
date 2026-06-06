#pragma once

// ════════════════════════════════════════════════════════════════════════════
// HomeKit service creation and sync — esp-homekit-sdk backend
// ════════════════════════════════════════════════════════════════════════════

#include "cn105_protocol.h"

extern "C" {
#include <hap.h>
}

// ── Service creation (called from homekit_setup during init) ────────────────

/// Create all HomeKit services (thermostat, fan, switches) and add to accessory.
void homekit_services_create_all(hap_acc_t *acc);

// ── Sync functions (called from main loop every ~2s) ────────────────────────

/// Push CN105 thermostat state to HomeKit characteristics.
void homekit_sync_thermostat(CN105Controller &cn105);

/// Push CN105 fan state to HomeKit characteristics.
void homekit_sync_fan(CN105Controller &cn105);

/// Push CN105 mode switch states to HomeKit characteristics.
void homekit_sync_switches(CN105Controller &cn105);

// ── BLE remote sensor battery (compiled only when BLE is enabled) ───────────
#include "ble_config.h"
#ifdef BLE_ENABLE
/// Create the HomeKit Battery Service for the BLE remote sensor.
void homekit_create_ble_battery(hap_acc_t *acc);
/// Push BLE sensor battery level + low-battery status to HomeKit.
void homekit_sync_ble_sensor(CN105Controller &cn105);
#endif

// ── Per-service creation (internal, called from create_all) ─────────────────

void homekit_create_thermostat(hap_acc_t *acc);
void homekit_create_fan(hap_acc_t *acc);
void homekit_create_fan_auto_switch(hap_acc_t *acc);
void homekit_create_fan_mode_switch(hap_acc_t *acc);
void homekit_create_dry_mode_switch(hap_acc_t *acc);

// ── CN105 controller reference (set during init, used by callbacks) ─────────
/// Must be set before any write callbacks fire.
void homekit_services_set_controller(CN105Controller *ctrl);
