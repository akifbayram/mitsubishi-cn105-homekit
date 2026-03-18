#include "homekit_services.h"
#include "settings.h"
#include "logging.h"
#include "compat_arduino.h"

#include <cstring>
#include <cmath>

extern "C" {
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
}

static const char *TAG = "hk_fan";

// ── HAP UUID not in SDK ─────────────────────────────────────────────────────
#define HAP_CHAR_UUID_CONFIGURED_NAME "E3"

// ── External controller reference ───────────────────────────────────────────
extern CN105Controller *g_homekitCtrl;

// ═══════════════════════════════════════════════════════════════════════════
// Fan Service
// ═══════════════════════════════════════════════════════════════════════════

static hap_char_t *s_fanActive       = nullptr;
static hap_char_t *s_fanSpeed        = nullptr;
static hap_char_t *s_fanStatusActive = nullptr;

static uint32_t s_fanLastSync = 0;
static bool     s_fanWasDisconnected = true;

// ── Mapping helpers ─────────────────────────────────────────────────────────

static uint8_t cn105FanToPercent(uint8_t fan) {
    switch (fan) {
        case CN105_FAN_AUTO:  return 0;
        case CN105_FAN_QUIET: return 10;
        case CN105_FAN_1:     return 30;
        case CN105_FAN_2:     return 50;
        case CN105_FAN_3:     return 70;
        case CN105_FAN_4:     return 100;
        default:              return 0;
    }
}

static uint8_t percentToCN105Fan(uint8_t pct) {
    if (pct <= 20)      return CN105_FAN_QUIET;
    if (pct <= 40)      return CN105_FAN_1;
    if (pct <= 60)      return CN105_FAN_2;
    if (pct <= 80)      return CN105_FAN_3;
    return CN105_FAN_4;
}

// ── Write callback ──────────────────────────────────────────────────────────

static int fan_write_cb(hap_write_data_t write_data[], int count,
                         void *serv_priv, void *write_priv)
{
    if (!g_homekitCtrl || !g_homekitCtrl->isHealthy()) {
        LOG_WARN("[HK:Fan] write REJECTED — CN105 not healthy");
        for (int i = 0; i < count; i++) {
            *(write_data[i].status) = HAP_STATUS_RES_BUSY;
        }
        return HAP_FAIL;
    }

    const CN105State st = g_homekitCtrl->getEffectiveState();
    int ret = HAP_SUCCESS;

    for (int i = 0; i < count; i++) {
        hap_write_data_t *w = &write_data[i];
        const char *uuid = hap_char_get_type_uuid(w->hc);

        if (!strcmp(uuid, HAP_CHAR_UUID_ACTIVE)) {
            uint8_t active = w->val.u;
            LOG_INFO("[HK:Fan] HomeKit -> active: %d", active);
            if (active == 0) {
                g_homekitCtrl->setPower(false);
            }
            hap_char_update_val(w->hc, &w->val);
            *(w->status) = HAP_STATUS_SUCCESS;

        } else if (!strcmp(uuid, HAP_CHAR_UUID_ROTATION_SPEED)) {
            float pct = w->val.f;
            uint8_t ipct = (uint8_t)pct;
            if (ipct == 0) {
                LOG_INFO("[HK:Fan] HomeKit -> speed: 0%% -> power off");
                g_homekitCtrl->setPower(false);
            } else if (st.power) {
                uint8_t fanByte = percentToCN105Fan(ipct);
                LOG_INFO("[HK:Fan] HomeKit -> speed: %d%% -> CN105 fan=0x%02X", ipct, fanByte);
                g_homekitCtrl->setFanSpeed(fanByte);
            } else {
                LOG_WARN("[HK:Fan] HomeKit -> speed: %d%% IGNORED (unit off)", ipct);
            }
            hap_char_update_val(w->hc, &w->val);
            *(w->status) = HAP_STATUS_SUCCESS;

        } else {
            *(w->status) = HAP_STATUS_RES_ABSENT;
        }
    }

    g_homekitCtrl->sendPendingChanges();
    return ret;
}

// ── Service creation ────────────────────────────────────────────────────────

void homekit_create_fan(hap_acc_t *acc)
{
    // Create Fan v2 service with Active characteristic
    hap_serv_t *serv = hap_serv_fan_v2_create(1);
    if (!serv) {
        LOG_ERROR("[HK:Fan] Failed to create fan service");
        return;
    }

    s_fanActive = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_ACTIVE);

    // Add RotationSpeed
    s_fanSpeed = hap_char_rotation_speed_create(0);
    hap_char_float_set_constraints(s_fanSpeed, 0.0f, 100.0f, 1.0f);
    hap_serv_add_char(serv, s_fanSpeed);

    // StatusActive
    s_fanStatusActive = hap_char_status_active_create(true);
    hap_serv_add_char(serv, s_fanStatusActive);

    // ConfiguredName
    hap_char_t *cname = hap_char_string_create(
        const_cast<char*>(HAP_CHAR_UUID_CONFIGURED_NAME),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV,
        const_cast<char*>("Fan"));
    hap_serv_add_char(serv, cname);

    hap_serv_set_write_cb(serv, fan_write_cb);
    hap_acc_add_serv(acc, serv);

    LOG_INFO("[HK:Fan] Service created (0-100%%, bands: Quiet/1/2/3/4)");
}

// Forward declaration for fan auto sync
static void sync_fan_auto(CN105Controller &cn105);

// ── Sync function ───────────────────────────────────────────────────────────

void homekit_sync_fan(CN105Controller &cn105)
{
    uint32_t now = millis();
    if (now - s_fanLastSync < 2000) return;
    s_fanLastSync = now;

    if (!cn105.isHealthy()) {
        s_fanWasDisconnected = true;
        if (s_fanStatusActive) {
            const hap_val_t *cur = hap_char_get_val(s_fanStatusActive);
            if (!cur || cur->b != false) {
                hap_val_t v; v.b = false;
                hap_char_update_val(s_fanStatusActive, &v);
            }
        }
        return;
    }

    const CN105State s = cn105.getEffectiveState();

    bool forceSync = s_fanWasDisconnected;
    if (s_fanWasDisconnected) {
        LOG_INFO("[HK:Fan] CN105 recovered — force syncing");
        s_fanWasDisconnected = false;
    }

    if (s_fanStatusActive) {
        const hap_val_t *cur = hap_char_get_val(s_fanStatusActive);
        if (!cur || cur->b != true) {
            hap_val_t v; v.b = true;
            hap_char_update_val(s_fanStatusActive, &v);
        }
    }

    // Active
    if (s_fanActive) {
        uint32_t active = s.power ? 1 : 0;
        const hap_val_t *cur = hap_char_get_val(s_fanActive);
        if (forceSync || !cur || cur->u != active) {
            LOG_DEBUG("[HK:Fan] sync active: %lu", (unsigned long)active);
            hap_val_t v = { .u = active };
            hap_char_update_val(s_fanActive, &v);
        }
    }

    // Speed (only when powered on)
    if (s_fanSpeed && s.power) {
        float pct = (float)cn105FanToPercent(s.fanSpeed);
        const hap_val_t *cur = hap_char_get_val(s_fanSpeed);
        if (forceSync || !cur || fabsf(cur->f - pct) > 0.5f) {
            LOG_DEBUG("[HK:Fan] sync speed: %.0f%%", pct);
            hap_val_t v = { .f = pct };
            hap_char_update_val(s_fanSpeed, &v);
        }
    }

    // Also sync the Fan Auto switch (same timing group)
    sync_fan_auto(cn105);
}

// ═══════════════════════════════════════════════════════════════════════════
// Fan Auto Switch
// ═══════════════════════════════════════════════════════════════════════════

static hap_char_t *s_fanAutoOn          = nullptr;
static hap_char_t *s_fanAutoStatusActive = nullptr;

static uint32_t s_fanAutoLastSync = 0;
static bool     s_fanAutoWasDisconnected = true;

static int fan_auto_write_cb(hap_write_data_t write_data[], int count,
                              void *serv_priv, void *write_priv)
{
    if (!g_homekitCtrl || !g_homekitCtrl->isHealthy()) {
        LOG_WARN("[HK:FanAuto] write REJECTED — CN105 not healthy");
        for (int i = 0; i < count; i++) {
            *(write_data[i].status) = HAP_STATUS_RES_BUSY;
        }
        return HAP_FAIL;
    }

    for (int i = 0; i < count; i++) {
        hap_write_data_t *w = &write_data[i];
        const char *uuid = hap_char_get_type_uuid(w->hc);

        if (!strcmp(uuid, HAP_CHAR_UUID_ON)) {
            const CN105State st = g_homekitCtrl->getEffectiveState();
            if (!st.power) {
                LOG_WARN("[HK:FanAuto] HomeKit -> toggle IGNORED (unit off)");
                hap_char_update_val(w->hc, &w->val);
                *(w->status) = HAP_STATUS_SUCCESS;
                continue;
            }

            bool autoMode = w->val.b;
            LOG_INFO("[HK:FanAuto] HomeKit -> auto: %s", autoMode ? "ON" : "OFF");
            if (autoMode) {
                g_homekitCtrl->setFanSpeed(CN105_FAN_AUTO);
            } else {
                g_homekitCtrl->setFanSpeed(CN105_FAN_2);
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

void homekit_create_fan_auto_switch(hap_acc_t *acc)
{
    hap_serv_t *serv = hap_serv_switch_create(true);
    if (!serv) {
        LOG_ERROR("[HK:FanAuto] Failed to create switch service");
        return;
    }

    s_fanAutoOn = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_ON);

    // ConfiguredName
    hap_char_t *cname = hap_char_string_create(
        const_cast<char*>(HAP_CHAR_UUID_CONFIGURED_NAME),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV,
        const_cast<char*>("Fan Auto"));
    hap_serv_add_char(serv, cname);

    s_fanAutoStatusActive = hap_char_status_active_create(true);
    hap_serv_add_char(serv, s_fanAutoStatusActive);

    hap_serv_set_write_cb(serv, fan_auto_write_cb);
    hap_acc_add_serv(acc, serv);

    LOG_INFO("[HK:FanAuto] Service created (toggle for auto fan speed)");
}

// Fan Auto sync — called from homekit_sync_fan to keep it grouped
static void sync_fan_auto(CN105Controller &cn105)
{
    uint32_t now = millis();
    if (now - s_fanAutoLastSync < 2000) return;
    s_fanAutoLastSync = now;

    if (!cn105.isHealthy()) {
        s_fanAutoWasDisconnected = true;
        if (s_fanAutoStatusActive) {
            const hap_val_t *cur = hap_char_get_val(s_fanAutoStatusActive);
            if (!cur || cur->b != false) {
                hap_val_t v; v.b = false;
                hap_char_update_val(s_fanAutoStatusActive, &v);
            }
        }
        return;
    }

    const CN105State s = cn105.getEffectiveState();

    bool forceSync = s_fanAutoWasDisconnected;
    if (s_fanAutoWasDisconnected) {
        LOG_INFO("[HK:FanAuto] CN105 recovered — force syncing");
        s_fanAutoWasDisconnected = false;
    }

    if (s_fanAutoStatusActive) {
        const hap_val_t *cur = hap_char_get_val(s_fanAutoStatusActive);
        if (!cur || cur->b != true) {
            hap_val_t v; v.b = true;
            hap_char_update_val(s_fanAutoStatusActive, &v);
        }
    }

    if (s_fanAutoOn) {
        bool isAuto = (s.fanSpeed == CN105_FAN_AUTO);
        const hap_val_t *cur = hap_char_get_val(s_fanAutoOn);
        if (forceSync || !cur || cur->b != isAuto) {
            LOG_DEBUG("[HK:FanAuto] sync auto: %s", isAuto ? "ON" : "OFF");
            hap_val_t v; v.b = isAuto;
            hap_char_update_val(s_fanAutoOn, &v);
        }
    }
}

