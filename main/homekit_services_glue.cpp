#include "homekit_services.h"
#include "logging.h"

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

    homekit_create_thermostat(acc);
    homekit_create_fan(acc);
    homekit_create_fan_auto_switch(acc);
    homekit_create_fan_mode_switch(acc);
    homekit_create_dry_mode_switch(acc);

    LOG_INFO("[HK] All HomeKit services created");
}

