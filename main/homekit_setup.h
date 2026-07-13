#pragma once

#include <cstdint>

// ════════════════════════════════════════════════════════════════════════════
// HomeKit (HAP) initialization and management — esp-homekit-sdk backend
// ════════════════════════════════════════════════════════════════════════════

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize HAP framework: create accessory, services, register callbacks,
/// and start the HAP HTTP server (port from CONFIG_HAP_HTTP_SERVER_PORT, 8080;
/// clients discover it via the _hap._tcp mDNS record).
/// Call after NVS, WiFi, and CN105 are initialized.
/// Returns true on success, false if HAP failed to start.
bool homekit_init(const char* name, const char* manufacturer,
                  const char* model, const char* serialNumber,
                  const char* fwRevision, const char* mdnsHostname);

/// Generate (or load from NVS) the 8-digit setup code, then set it on HAP.
void homekit_generate_setup_code(void);

/// Get the X-HM:// setup payload URI (for QR code). Caller must NOT free.
const char* homekit_get_setup_payload(void);

/// Get the formatted setup code (XXX-XX-XXX). Caller must NOT free.
const char* homekit_get_setup_code(void);

/// Get the number of paired controllers.
int homekit_get_controller_count(void);

/// Remove all HomeKit pairings and reboot. Returns false when HomeKit isn't
/// running (nothing was reset); on success the device reboots shortly after.
bool homekit_reset_pairings(void);

/// Get a human-readable status string reflecting the current HAP state.
const char* homekit_get_status_string(void);

/// True once homekit_init() has fully succeeded (HAP server running).
bool homekit_is_started(void);

// Mode capability mask the HAP accessory database was built with at boot.
// Differs from settings.get().modeMask after a web-UI change until restart.
uint8_t homekit_get_boot_mode_mask(void);

/// Reconcile the live HAP service shape (mode-gated services + Remote Sensor
/// accessory presence) against the shape recorded in NVS, bumping the HAP
/// config number on mismatch so paired Home apps refresh their cached
/// accessory database. Runs after hap_start(); call again whenever the shape
/// changes at runtime (e.g. the sensor accessory is created). No-op before
/// HomeKit has started.
void homekit_reconcile_service_shape(void);

#ifdef __cplusplus
}
#endif
