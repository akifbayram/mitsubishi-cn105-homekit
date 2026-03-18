#pragma once

// ════════════════════════════════════════════════════════════════════════════
// HomeKit stub functions — placeholders until Task 11 implements real HAP
// ════════════════════════════════════════════════════════════════════════════

#ifdef __cplusplus
extern "C" {
#endif

/// Number of paired HomeKit controllers (stub: always 0)
int homekit_get_controller_count(void);

/// Human-readable HomeKit status string (stub: "Not Ready")
const char* homekit_get_status_string(void);

/// X-HM:// setup payload URI for QR code (stub: empty)
const char* homekit_get_setup_payload(void);

/// Formatted setup code XXX-XX-XXX (stub: "000-00-000")
const char* homekit_get_setup_code(void);

/// Remove all HomeKit pairings and restart (stub: no-op)
void homekit_reset_pairings(void);

#ifdef __cplusplus
}
#endif
