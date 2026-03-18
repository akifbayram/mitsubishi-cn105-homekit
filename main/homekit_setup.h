#pragma once

// ════════════════════════════════════════════════════════════════════════════
// HomeKit (HAP) initialization and management — esp-homekit-sdk backend
// ════════════════════════════════════════════════════════════════════════════

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize HAP framework: create accessory, services, register callbacks.
/// Call after NVS, WiFi, and CN105 are initialized.
void homekit_init(const char* name, const char* manufacturer,
                  const char* model, const char* serialNumber,
                  const char* fwRevision);

/// Generate (or load from NVS) the 8-digit setup code, then set it on HAP.
void homekit_generate_setup_code(void);

/// Set the 4-character setup ID (for QR code generation).
void homekit_set_setup_id(const char* setupId);

/// Get the X-HM:// setup payload URI (for QR code). Caller must NOT free.
const char* homekit_get_setup_payload(void);

/// Get the formatted setup code (XXX-XX-XXX). Caller must NOT free.
const char* homekit_get_setup_code(void);

/// Get the number of paired controllers.
int homekit_get_controller_count(void);

/// Remove all HomeKit pairings and reboot.
void homekit_reset_pairings(void);

/// Reset HomeKit network credentials and reboot.
void homekit_reset_network(void);

/// Get a human-readable status string reflecting the current HAP state.
const char* homekit_get_status_string(void);

#ifdef __cplusplus
}
#endif
