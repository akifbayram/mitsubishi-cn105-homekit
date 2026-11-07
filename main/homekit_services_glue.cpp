#include "homekit_services.h"
#include "logging.h"
#include "settings.h"
#include "homekit_setup.h"

extern "C" {
#include <hap_apple_servs.h>
}

static const char *TAG = "hk_svc";

// ── Global controller reference used by all write callbacks ─────────────────
CN105Controller *g_homekitCtrl = nullptr;

void homekit_services_set_controller(CN105Controller *ctrl)
{
    g_homekitCtrl = ctrl;
}

// ── Create all services ─────────────────────────────────────────────────────

// Moves a built service and its characteristics onto its fixed slot.
static void pin_slot(hap_serv_t *serv, int32_t thermostatLastIid, HkSlot slot)
{
    static const char *const NAMES[HK_SLOT_COUNT] = {"Fan", "Fan Auto", "Fan Mode", "Dry Mode", "Vane Preset"};
    if (!serv) return;
    int32_t iid = hk_slot_first_iid(thermostatLastIid, slot);
    hap_serv_set_iid(serv, iid++);
    int count = 1;
    for (hap_char_t *c = hap_serv_get_first_char(serv); c; c = hap_char_get_next(c), ++count)
        hap_char_set_iid(c, iid++);
    if (count != HK_SLOT_SIZE[slot])
        LOG_ERROR("[HK] %s has %d IIDs but its slot holds %u", NAMES[slot], count, (unsigned)HK_SLOT_SIZE[slot]);
}

void homekit_services_create_all(hap_acc_t *acc)
{
    LOG_INFO("[HK] Creating HomeKit services...");

    // Mode-gated and owner-hidden services are decided at boot; a change needs
    // a restart (HAP services can't be added/removed after hap_start()).
    uint8_t mask = settings.get().modeMask;
    const uint8_t built = hk_services_built(homekit_get_boot_hidden(), mask);

    homekit_create_thermostat(acc);
    hap_serv_t *slots[HK_SLOT_COUNT] = {};
    if (built & HK_SVC_FAN) {
        slots[HK_SLOT_FAN] = homekit_create_fan(acc);
    } else {
        LOG_INFO("[HK] Fan hidden — service not created");
    }
    if (built & HK_SVC_FAN_AUTO) {
        slots[HK_SLOT_FAN_AUTO] = homekit_create_fan_auto_switch(acc);
    } else {
        LOG_INFO("[HK] Fan Auto hidden — switch not created");
    }
    if (!(mask & MODE_CAP_FAN)) {
        LOG_INFO("[HK] FAN mode disabled by capability mask — switch not created");
    } else if (!(built & HK_SVC_FAN_MODE)) {
        LOG_INFO("[HK] Fan Mode hidden — switch not created");
    } else {
        slots[HK_SLOT_FAN_MODE] = homekit_create_fan_mode_switch(acc);
    }
    if (!(mask & MODE_CAP_DRY)) {
        LOG_INFO("[HK] DRY mode disabled by capability mask — switch not created");
    } else if (!(built & HK_SVC_DRY_MODE)) {
        LOG_INFO("[HK] Dry Mode hidden — switch not created");
    } else {
        slots[HK_SLOT_DRY_MODE] = homekit_create_dry_mode_switch(acc);
    }
    // Last slot, so enabling it never moves the service IDs Home scenes are bound to.
    if (homekit_get_boot_vane_switch()) {
        slots[HK_SLOT_VANE_PRESET] = homekit_create_vane_switch(acc);
    } else {
        LOG_INFO("[HK] Vane Switch off (or no vanes) — switch not created");
    }

    // The SDK numbers IIDs in creation order, so a missing service would hand
    // its IIDs to the next one. Each optional service instead takes its fixed
    // slot (hk_service_slots.h), counted from the thermostat's last IID so
    // Range and Single keep today's numbers. Set after every service is added
    // and before hap_start() (hap.h, hap_serv_set_iid()).
    hap_serv_t *thermostat = hap_acc_get_serv_by_uuid(acc, HAP_SERV_UUID_THERMOSTAT);
    if (!thermostat) {
        LOG_ERROR("[HK] No thermostat service — optional service IIDs not pinned");
    } else {
        uint32_t last = hap_serv_get_iid(thermostat);
        for (hap_char_t *c = hap_serv_get_first_char(thermostat); c; c = hap_char_get_next(c))
            if (hap_char_get_iid(c) > last) last = hap_char_get_iid(c);
        for (int s = 0; s < HK_SLOT_COUNT; ++s) pin_slot(slots[s], (int32_t)last, HkSlot(s));
        LOG_INFO("[HK] Optional service IIDs pinned from thermostat IID %lu (built 0x%02X)",
                 (unsigned long)last, (unsigned)built);
    }

    LOG_INFO("[HK] All HomeKit services created");
}
