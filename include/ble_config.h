#pragma once

// ── Auto-enable BLE on Bluetooth-capable boards ────────────────────────────
// Include this header anywhere that needs to check #ifdef BLE_ENABLE.
// Opt out with -DBLE_DISABLE in build flags.

#if defined(CONFIG_BT_ENABLED) && !defined(BLE_DISABLE)
#define BLE_ENABLE
#endif

// Legacy support: if BLE_SENSOR_TYPE was defined, enable BLE
#if defined(BLE_SENSOR_TYPE) && !defined(BLE_ENABLE)
#define BLE_ENABLE
#endif
