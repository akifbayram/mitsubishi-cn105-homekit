#include "homekit_services.h"
#include "settings.h"
#include "setpoint_control.h"
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
    const bool healthy = g_homekitCtrl && g_homekitCtrl->isHealthy();
    SetpointControl::Command batch;
    for (int i = 0; i < count; ++i) {
        const auto& w = write_data[i];
        if (!strcmp(hap_char_get_type_uuid(w.hc), HAP_CHAR_UUID_ON)) {
            batch.unit.power = w.val.b;
            if (w.val.b) batch.unit.mode = CN105_MODE_FAN;
        }
    }
    const auto result = healthy ? SetpointControl::submit(batch) : SetpointRange::Result::Unavailable;
    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; ++i) {
        auto& w = write_data[i];
        const char* uuid = hap_char_get_type_uuid(w.hc);
        if (!strcmp(uuid, HAP_CHAR_UUID_CONFIGURED_NAME)) {
            hap_char_update_val(w.hc, &w.val); *w.status = HAP_STATUS_SUCCESS;
        } else if (strcmp(uuid, HAP_CHAR_UUID_ON)) *w.status = HAP_STATUS_RES_ABSENT;
        else if (result != SetpointRange::Result::Ok) {
            *w.status = healthy ? HAP_STATUS_VAL_INVALID : HAP_STATUS_RES_BUSY; ret = HAP_FAIL;
        } else { hap_char_update_val(w.hc, &w.val); *w.status = HAP_STATUS_SUCCESS; }
    }
    return ret;
}

hap_serv_t *homekit_create_fan_mode_switch(hap_acc_t *acc)
{
    hap_serv_t *serv = hap_serv_switch_create(false);
    if (!serv) {
        LOG_ERROR("[HK:FanMode] Failed to create switch service");
        return nullptr;
    }

    s_fanModeOn = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_ON);

    // Name (default label shown by Home app at pairing) + ConfiguredName (rename).
    // Both are required: without Name, multiple same-type services show as
    // "Switch", "Switch 2", "Switch 3" until a later re-read picks up ConfiguredName.
    hap_serv_add_char(serv, hap_char_name_create(const_cast<char*>("Fan Mode")));

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
    return serv;
}

static void sync_fan_mode(CN105Controller &cn105)
{
    if (!s_fanModeOn) return;   // not built this boot (modeMask or hkHidden)
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
        bool isFanMode = s.power && (s.mode == CN105_MODE_FAN);
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
    const bool healthy = g_homekitCtrl && g_homekitCtrl->isHealthy();
    SetpointControl::Command batch;
    for (int i = 0; i < count; ++i) {
        const auto& w = write_data[i];
        if (!strcmp(hap_char_get_type_uuid(w.hc), HAP_CHAR_UUID_ON)) {
            batch.unit.power = w.val.b;
            if (w.val.b) batch.unit.mode = CN105_MODE_DRY;
        }
    }
    const auto result = healthy ? SetpointControl::submit(batch) : SetpointRange::Result::Unavailable;
    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; ++i) {
        auto& w = write_data[i];
        const char* uuid = hap_char_get_type_uuid(w.hc);
        if (!strcmp(uuid, HAP_CHAR_UUID_CONFIGURED_NAME)) {
            hap_char_update_val(w.hc, &w.val); *w.status = HAP_STATUS_SUCCESS;
        } else if (strcmp(uuid, HAP_CHAR_UUID_ON)) *w.status = HAP_STATUS_RES_ABSENT;
        else if (result != SetpointRange::Result::Ok) {
            *w.status = healthy ? HAP_STATUS_VAL_INVALID : HAP_STATUS_RES_BUSY; ret = HAP_FAIL;
        } else { hap_char_update_val(w.hc, &w.val); *w.status = HAP_STATUS_SUCCESS; }
    }
    return ret;
}

hap_serv_t *homekit_create_dry_mode_switch(hap_acc_t *acc)
{
    hap_serv_t *serv = hap_serv_switch_create(false);
    if (!serv) {
        LOG_ERROR("[HK:DryMode] Failed to create switch service");
        return nullptr;
    }

    s_dryModeOn = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_ON);

    hap_serv_add_char(serv, hap_char_name_create(const_cast<char*>("Dry Mode")));

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
    return serv;
}

static void sync_dry_mode(CN105Controller &cn105)
{
    if (!s_dryModeOn) return;   // not built this boot (modeMask or hkHidden)
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
        bool isDryMode = s.power && (s.mode == CN105_MODE_DRY);
        const hap_val_t *cur = hap_char_get_val(s_dryModeOn);
        if (forceSync || !cur || cur->b != isDryMode) {
            LOG_DEBUG("[HK:DryMode] sync: %s", isDryMode ? "ON" : "OFF");
            hap_val_t v; v.b = isDryMode;
            hap_char_update_val(s_dryModeOn, &v);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Vane Switch (vane_preset.h, CONTEXT.md › Airflow)
// ═══════════════════════════════════════════════════════════════════════════
// Opt-in. On applies the owner's On preset, Off applies the Off preset —
// vanes only, never power, mode, fan or setpoint. The On characteristic
// mirrors the unit: true only while the current vanes match the On preset
// under the current Vane Type, so a remote-control change flips it to Off.
// Presets are read from settings on every write and sync, so an edit in the
// web UI takes effect without a restart; only the service's existence is
// fixed at boot (homekit_services_glue.cpp).

static hap_char_t *s_vaneOn           = nullptr;
static hap_char_t *s_vaneStatusActive = nullptr;

static uint32_t s_vaneLastSync = 0;
static bool     s_vaneWasDisconnected = true;

static int vane_switch_write_cb(hap_write_data_t write_data[], int count,
                                void *serv_priv, void *write_priv)
{
    const bool healthy = g_homekitCtrl && g_homekitCtrl->isHealthy();
    const DeviceSettings& cfg = settings.get();
    // The service was built because the unit had vanes at boot; if Vane Type
    // was set to None since, there is nothing to drive until the restart
    // that removes the service.
    const bool driveable = cfg.vaneConfig != VANE_TYPE_NONE;
    SetpointControl::Command batch;
    bool wantsOn = false;
    VanePreset p = cfg.vaneOff;
    for (int i = 0; i < count; ++i) {
        const auto& w = write_data[i];
        if (!strcmp(hap_char_get_type_uuid(w.hc), HAP_CHAR_UUID_ON)) {
            wantsOn = w.val.b;
            p = wantsOn ? cfg.vaneOn : cfg.vaneOff;
            batch.unit.vane = p.vane;
            if (vane_preset_wide_applies(cfg.vaneConfig)) batch.unit.wideVane = p.wideVane;
        }
    }
    const auto result = !healthy ? SetpointRange::Result::Unavailable
                      : !driveable ? SetpointRange::Result::Invalid
                      : SetpointControl::submit(batch);
    if (result == SetpointRange::Result::Ok)
        LOG_INFO("[HK:Vane] %s preset sent (vane=%u wide=%u)", wantsOn ? "On" : "Off",
                 (unsigned)p.vane, (unsigned)p.wideVane);
    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; ++i) {
        auto& w = write_data[i];
        const char* uuid = hap_char_get_type_uuid(w.hc);
        if (!strcmp(uuid, HAP_CHAR_UUID_CONFIGURED_NAME)) {
            hap_char_update_val(w.hc, &w.val); *w.status = HAP_STATUS_SUCCESS;
        } else if (strcmp(uuid, HAP_CHAR_UUID_ON)) *w.status = HAP_STATUS_RES_ABSENT;
        else if (result != SetpointRange::Result::Ok) {
            *w.status = healthy ? HAP_STATUS_VAL_INVALID : HAP_STATUS_RES_BUSY; ret = HAP_FAIL;
        } else { hap_char_update_val(w.hc, &w.val); *w.status = HAP_STATUS_SUCCESS; }
    }
    return ret;
}

hap_serv_t *homekit_create_vane_switch(hap_acc_t *acc)
{
    hap_serv_t *serv = hap_serv_switch_create(false);
    if (!serv) {
        LOG_ERROR("[HK:Vane] Failed to create switch service");
        return nullptr;
    }

    s_vaneOn = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_ON);

    hap_serv_add_char(serv, hap_char_name_create(const_cast<char*>("Vane Preset")));

    hap_char_t *cname = hap_char_string_create(
        const_cast<char*>(HAP_CHAR_UUID_CONFIGURED_NAME),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV,
        const_cast<char*>("Vane Preset"));
    hap_serv_add_char(serv, cname);

    s_vaneStatusActive = hap_char_status_active_create(true);
    hap_serv_add_char(serv, s_vaneStatusActive);

    hap_serv_set_write_cb(serv, vane_switch_write_cb);
    hap_acc_add_serv(acc, serv);

    LOG_INFO("[HK:Vane] Service created (Vane Switch, shown in Home as Vane Preset)");
    return serv;
}

static void sync_vane_switch(CN105Controller &cn105)
{
    if (!s_vaneOn) return;   // not built this boot
    uint32_t now = uptime_ms();
    if (now - s_vaneLastSync < 2000) return;
    s_vaneLastSync = now;

    if (!cn105.isHealthy()) {
        s_vaneWasDisconnected = true;
        if (s_vaneStatusActive) {
            const hap_val_t *cur = hap_char_get_val(s_vaneStatusActive);
            if (!cur || cur->b != false) {
                hap_val_t v; v.b = false;
                hap_char_update_val(s_vaneStatusActive, &v);
            }
        }
        return;
    }

    const CN105State s = cn105.getEffectiveState();

    bool forceSync = s_vaneWasDisconnected;
    if (s_vaneWasDisconnected) {
        LOG_INFO("[HK:Vane] CN105 recovered — force syncing");
        s_vaneWasDisconnected = false;
    }

    if (s_vaneStatusActive) {
        const hap_val_t *cur = hap_char_get_val(s_vaneStatusActive);
        if (!cur || cur->b != true) {
            hap_val_t v; v.b = true;
            hap_char_update_val(s_vaneStatusActive, &v);
        }
    }

    const DeviceSettings& cfg = settings.get();
    bool isOn = vane_preset_matches(cfg.vaneOn, s.vane, s.wideVane, cfg.vaneConfig);
    const hap_val_t *cur = hap_char_get_val(s_vaneOn);
    if (forceSync || !cur || cur->b != isOn) {
        LOG_DEBUG("[HK:Vane] sync: %s", isOn ? "ON" : "OFF");
        hap_val_t v; v.b = isOn;
        hap_char_update_val(s_vaneOn, &v);
    }
}

// ── Public sync function ────────────────────────────────────────────────────

void homekit_sync_switches(CN105Controller &cn105)
{
    sync_fan_mode(cn105);
    sync_dry_mode(cn105);
    sync_vane_switch(cn105);
}
