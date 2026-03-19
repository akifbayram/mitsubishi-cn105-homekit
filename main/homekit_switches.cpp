#include "homekit_services.h"
#include "settings.h"
#include "logging.h"
#include "esp_utils.h"

#include <cstring>

extern "C" {
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
}

static const char *TAG = "hk_switch";

// ── HAP UUID not in SDK ─────────────────────────────────────────────────────
#define HAP_CHAR_UUID_CONFIGURED_NAME "E3"

// ── External controller reference ───────────────────────────────────────────
extern CN105Controller *g_homekitCtrl;

// ═══════════════════════════════════════════════════════════════════════════
// Fan Mode Switch
// ═══════════════════════════════════════════════════════════════════════════

static hap_char_t *s_fanModeOn          = nullptr;
static hap_char_t *s_fanModeStatusActive = nullptr;

static uint32_t s_fanModeLastSync = 0;
static bool     s_fanModeWasDisconnected = true;

static int fan_mode_write_cb(hap_write_data_t write_data[], int count,
                              void *serv_priv, void *write_priv)
{
    if (!g_homekitCtrl || !g_homekitCtrl->isHealthy()) {
        LOG_WARN("[HK:FanMode] write REJECTED — CN105 not healthy");
        for (int i = 0; i < count; i++) {
            *(write_data[i].status) = HAP_STATUS_RES_BUSY;
        }
        return HAP_FAIL;
    }

    for (int i = 0; i < count; i++) {
        hap_write_data_t *w = &write_data[i];
        const char *uuid = hap_char_get_type_uuid(w->hc);

        if (!strcmp(uuid, HAP_CHAR_UUID_ON)) {
            bool on = w->val.b;
            if (on) {
                LOG_INFO("[HK:FanMode] HomeKit -> ON");
                g_homekitCtrl->setPower(true);
                g_homekitCtrl->setMode(CN105_MODE_FAN);
            } else {
                LOG_INFO("[HK:FanMode] HomeKit -> OFF (powering off)");
                g_homekitCtrl->setPower(false);
            }
            hap_char_update_val(w->hc, &w->val);
            *(w->status) = HAP_STATUS_SUCCESS;
        } else {
            *(w->status) = HAP_STATUS_RES_ABSENT;
        }
    }

    g_homekitCtrl->sendPendingChanges();
    return HAP_SUCCESS;
}

void homekit_create_fan_mode_switch(hap_acc_t *acc)
{
    hap_serv_t *serv = hap_serv_switch_create(false);
    if (!serv) {
        LOG_ERROR("[HK:FanMode] Failed to create switch service");
        return;
    }

    s_fanModeOn = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_ON);

    hap_char_t *cname = hap_char_string_create(
        const_cast<char*>(HAP_CHAR_UUID_CONFIGURED_NAME),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV,
        const_cast<char*>("Fan Mode"));
    hap_serv_add_char(serv, cname);

    s_fanModeStatusActive = hap_char_status_active_create(true);
    hap_serv_add_char(serv, s_fanModeStatusActive);

    hap_serv_set_write_cb(serv, fan_mode_write_cb);
    hap_acc_add_serv(acc, serv);

    LOG_INFO("[HK:FanMode] Service created (FAN mode switch)");
}

static void sync_fan_mode(CN105Controller &cn105)
{
    uint32_t now = uptime_ms();
    if (now - s_fanModeLastSync < 2000) return;
    s_fanModeLastSync = now;

    if (!cn105.isHealthy()) {
        s_fanModeWasDisconnected = true;
        if (s_fanModeStatusActive) {
            const hap_val_t *cur = hap_char_get_val(s_fanModeStatusActive);
            if (!cur || cur->b != false) {
                hap_val_t v; v.b = false;
                hap_char_update_val(s_fanModeStatusActive, &v);
            }
        }
        return;
    }

    const CN105State s = cn105.getEffectiveState();

    bool forceSync = s_fanModeWasDisconnected;
    if (s_fanModeWasDisconnected) {
        LOG_INFO("[HK:FanMode] CN105 recovered — force syncing");
        s_fanModeWasDisconnected = false;
    }

    if (s_fanModeStatusActive) {
        const hap_val_t *cur = hap_char_get_val(s_fanModeStatusActive);
        if (!cur || cur->b != true) {
            hap_val_t v; v.b = true;
            hap_char_update_val(s_fanModeStatusActive, &v);
        }
    }

    if (s_fanModeOn) {
        bool isFanMode = (s.power && s.mode == CN105_MODE_FAN);
        const hap_val_t *cur = hap_char_get_val(s_fanModeOn);
        if (forceSync || !cur || cur->b != isFanMode) {
            LOG_DEBUG("[HK:FanMode] sync: %s", isFanMode ? "ON" : "OFF");
            hap_val_t v; v.b = isFanMode;
            hap_char_update_val(s_fanModeOn, &v);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Dry Mode Switch
// ═══════════════════════════════════════════════════════════════════════════

static hap_char_t *s_dryModeOn          = nullptr;
static hap_char_t *s_dryModeStatusActive = nullptr;

static uint32_t s_dryModeLastSync = 0;
static bool     s_dryModeWasDisconnected = true;

static int dry_mode_write_cb(hap_write_data_t write_data[], int count,
                              void *serv_priv, void *write_priv)
{
    if (!g_homekitCtrl || !g_homekitCtrl->isHealthy()) {
        LOG_WARN("[HK:DryMode] write REJECTED — CN105 not healthy");
        for (int i = 0; i < count; i++) {
            *(write_data[i].status) = HAP_STATUS_RES_BUSY;
        }
        return HAP_FAIL;
    }

    for (int i = 0; i < count; i++) {
        hap_write_data_t *w = &write_data[i];
        const char *uuid = hap_char_get_type_uuid(w->hc);

        if (!strcmp(uuid, HAP_CHAR_UUID_ON)) {
            bool on = w->val.b;
            if (on) {
                LOG_INFO("[HK:DryMode] HomeKit -> ON");
                g_homekitCtrl->setPower(true);
                g_homekitCtrl->setMode(CN105_MODE_DRY);
            } else {
                LOG_INFO("[HK:DryMode] HomeKit -> OFF (powering off)");
                g_homekitCtrl->setPower(false);
            }
            hap_char_update_val(w->hc, &w->val);
            *(w->status) = HAP_STATUS_SUCCESS;
        } else {
            *(w->status) = HAP_STATUS_RES_ABSENT;
        }
    }

    g_homekitCtrl->sendPendingChanges();
    return HAP_SUCCESS;
}

void homekit_create_dry_mode_switch(hap_acc_t *acc)
{
    hap_serv_t *serv = hap_serv_switch_create(false);
    if (!serv) {
        LOG_ERROR("[HK:DryMode] Failed to create switch service");
        return;
    }

    s_dryModeOn = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_ON);

    hap_char_t *cname = hap_char_string_create(
        const_cast<char*>(HAP_CHAR_UUID_CONFIGURED_NAME),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV,
        const_cast<char*>("Dry Mode"));
    hap_serv_add_char(serv, cname);

    s_dryModeStatusActive = hap_char_status_active_create(true);
    hap_serv_add_char(serv, s_dryModeStatusActive);

    hap_serv_set_write_cb(serv, dry_mode_write_cb);
    hap_acc_add_serv(acc, serv);

    LOG_INFO("[HK:DryMode] Service created (DRY mode switch)");
}

static void sync_dry_mode(CN105Controller &cn105)
{
    uint32_t now = uptime_ms();
    if (now - s_dryModeLastSync < 2000) return;
    s_dryModeLastSync = now;

    if (!cn105.isHealthy()) {
        s_dryModeWasDisconnected = true;
        if (s_dryModeStatusActive) {
            const hap_val_t *cur = hap_char_get_val(s_dryModeStatusActive);
            if (!cur || cur->b != false) {
                hap_val_t v; v.b = false;
                hap_char_update_val(s_dryModeStatusActive, &v);
            }
        }
        return;
    }

    const CN105State s = cn105.getEffectiveState();

    bool forceSync = s_dryModeWasDisconnected;
    if (s_dryModeWasDisconnected) {
        LOG_INFO("[HK:DryMode] CN105 recovered — force syncing");
        s_dryModeWasDisconnected = false;
    }

    if (s_dryModeStatusActive) {
        const hap_val_t *cur = hap_char_get_val(s_dryModeStatusActive);
        if (!cur || cur->b != true) {
            hap_val_t v; v.b = true;
            hap_char_update_val(s_dryModeStatusActive, &v);
        }
    }

    if (s_dryModeOn) {
        bool isDryMode = (s.power && s.mode == CN105_MODE_DRY);
        const hap_val_t *cur = hap_char_get_val(s_dryModeOn);
        if (forceSync || !cur || cur->b != isDryMode) {
            LOG_DEBUG("[HK:DryMode] sync: %s", isDryMode ? "ON" : "OFF");
            hap_val_t v; v.b = isDryMode;
            hap_char_update_val(s_dryModeOn, &v);
        }
    }
}

// ── Public sync function ────────────────────────────────────────────────────

void homekit_sync_switches(CN105Controller &cn105)
{
    sync_fan_mode(cn105);
    sync_dry_mode(cn105);
}
