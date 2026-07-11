#include "homekit_services.h"
#include "logging.h"
#include "settings.h"

static const char *TAG = "hk_svc";

// ── Global controller reference used by all write callbacks ─────────────────
CN105Controller *g_homekitCtrl = nullptr;

void homekit_services_set_controller(CN105Controller *ctrl)
{
    g_homekitCtrl = ctrl;
}

// ── Create all services ─────────────────────────────────────────────────────

void homekit_services_create_all(hap_acc_t *acc)
{
    LOG_INFO("[HK] Creating HomeKit services...");

    // Mode-gated services are decided at boot; a mask change needs a restart
    // (HAP services can't be added/removed after hap_start()).
    uint8_t mask = settings.get().modeMask;

    homekit_create_thermostat(acc);
    homekit_create_fan(acc);
    homekit_create_fan_auto_switch(acc);
    if (mask & MODE_CAP_FAN) {
        homekit_create_fan_mode_switch(acc);
    } else {
        LOG_INFO("[HK] FAN mode disabled by capability mask — switch not created");
    }
    if (mask & MODE_CAP_DRY) {
        homekit_create_dry_mode_switch(acc);
    } else {
        LOG_INFO("[HK] DRY mode disabled by capability mask — switch not created");
    }

    LOG_INFO("[HK] All HomeKit services created");
}

