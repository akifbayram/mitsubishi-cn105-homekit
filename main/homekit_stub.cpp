#include "homekit_stub.h"

// ════════════════════════════════════════════════════════════════════════════
// HomeKit stub implementations — will be replaced by Task 11
// ════════════════════════════════════════════════════════════════════════════

int homekit_get_controller_count(void) {
    return 0;
}

const char* homekit_get_status_string(void) {
    return "Not Ready";
}

const char* homekit_get_setup_payload(void) {
    return "";
}

const char* homekit_get_setup_code(void) {
    return "000-00-000";
}

void homekit_reset_pairings(void) {
    // No-op until Task 11 implements real HomeKit HAP
}
