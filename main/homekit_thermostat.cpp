#include "homekit_services.h"
#include "homekit_setup.h"
#include "cn105_strings.h"
#include "auto_band.h"
#include "setpoint_control.h"
#include "room_avg.h"
#include "settings.h"
#include "logging.h"
#include "esp_utils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

extern "C" {
#include <hap.h>
#include <hap_platform_httpd.h>
#include <esp_http_server.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
}

static const char *TAG = "hk_thermo";

// ── HAP UUID not in SDK ─────────────────────────────────────────────────────
#define HAP_CHAR_UUID_CONFIGURED_NAME "E3"

// ── File-scope characteristic handles ───────────────────────────────────────
static hap_char_t *s_currentState  = nullptr;
static hap_char_t *s_targetState   = nullptr;
static hap_char_t *s_currentTemp   = nullptr;
static hap_char_t *s_targetTemp    = nullptr;
static hap_char_t *s_tempUnits     = nullptr;
static hap_char_t *s_statusFault   = nullptr;
static hap_char_t *s_heatThresh    = nullptr;
static hap_char_t *s_coolThresh    = nullptr;
static hap_char_t *s_statusActive  = nullptr;

// ── Sync state ──────────────────────────────────────────────────────────────
static uint32_t s_lastSync = 0;
static bool     s_wasDisconnected = true;

// ── External controller reference ───────────────────────────────────────────
extern CN105Controller *g_homekitCtrl;

// ── Debug helpers ───────────────────────────────────────────────────────────
static const char* hkTargetStateStr(uint8_t s) {
    switch (s) {
        case 0: return "OFF";
        case 1: return "HEAT";
        case 2: return "COOL";
        case 3: return "AUTO";
        default: return "?";
    }
}

static const char* hkCurrentStateStr(uint8_t s) {
    switch (s) {
        case 0: return "IDLE";
        case 1: return "HEATING";
        case 2: return "COOLING";
        default: return "?";
    }
}

// ── Mapping helpers ─────────────────────────────────────────────────────────

static uint8_t cn105ToHKTargetState(bool power, uint8_t mode) {
    if (!power) return 0;
    switch (mode) {
        case CN105_MODE_HEAT: return 1;
        case CN105_MODE_COOL: return 2;
        case CN105_MODE_AUTO: return 3;
        default:              return 0;
    }
}

static uint8_t hkTargetStateToCN105Mode(uint8_t hkState) {
    switch (hkState) {
        case 1:  return CN105_MODE_HEAT;
        case 2:  return CN105_MODE_COOL;
        case 3:  return CN105_MODE_AUTO;
        default: return CN105_MODE_AUTO;
    }
}

static uint8_t deriveCurrentState(const CN105State &s) {
    if (!s.power) return 0;

    if (s.operating) {
        switch (s.mode) {
            case CN105_MODE_HEAT: return 1;
            case CN105_MODE_COOL: return 2;
            case CN105_MODE_AUTO:
                if (s.autoSubMode == CN105_AUTOSUB_HEAT) return 1;
                if (s.autoSubMode == CN105_AUTOSUB_COOL) return 2;
                if (s.roomTemp < s.targetTemp - 0.5f) return 1;
                if (s.roomTemp > s.targetTemp + 0.5f) return 2;
                // Ambiguous (room ~= setpoint, no autoSubMode): guess by which
                // half of the AUTO band the room sits in — near the heating
                // threshold the unit was almost certainly heating, and vice versa.
                {
                    const auto band = SetpointControl::settingsView().effectiveBand;
                    float bandMid = (band.heat2 + band.cool2) * 0.25f;
                    return (s.roomTemp <= bandMid) ? 1 : 2;
                }
            default: return 0;
        }
    }

    return 0;
}

// HAP has no public characteristic lock. Every live thermostat cache/constraint
// update runs in its HTTP task, alongside reads and writes. Initialization runs
// directly before the accessory is exposed.
static void publish_thermostat(CN105Controller& cn105);
static bool s_targetReadable = true;
static std::atomic<bool> s_publicationQueued{false};

static bool representable(float value, int16_t low2, int16_t high2)
{
    return std::isfinite(value) && value * 2 >= low2 && value * 2 <= high2;
}

static void install_temperature(hap_char_t* hc, int16_t low2, int16_t high2,
                                uint8_t step2, float value)
{
    // No HTTP reader can interleave the constraints/cache pair. The SDK setter
    // changes constraints only; its update API enforces the newly installed bounds.
    hap_char_float_set_constraints(hc, low2 / 2.f, high2 / 2.f, step2 / 2.f);
    hap_val_t v{.f = value};
    hap_char_update_val(hc, &v);
}

static void install_temperature_metadata(CN105Controller* cn105)
{
    if (!s_targetTemp || !s_heatThresh || !s_coolThresh) return;
    const auto view = SetpointControl::settingsView();
    const auto& ranges = view.ranges;
    const auto& automatic = ranges.mode[2];
    const HomekitTemperatureMetadata metadata{
        {ranges.advertised.min2, automatic.range.min2, automatic.range.min2},
        {ranges.advertised.max2, automatic.range.max2, automatic.range.max2},
        {ranges.advertisedStep2, automatic.step2, automatic.step2}};
    const auto& status = view.status;
    const bool failed = status.condition == SetpointControl::Condition::Refused ||
                        status.condition == SetpointControl::Condition::Incompatible;
    float target = cn105 ? cn105->getEffectiveState().targetTemp : NAN;
    if (failed) target = status.actualC;
    else if (status.condition == SetpointControl::Condition::Pending &&
             !representable(target, metadata.min2[0], metadata.max2[0])) target = status.desiredC;
    s_targetReadable = representable(target, metadata.min2[0], metadata.max2[0]);
    if (!s_targetReadable) {
        // /accessories ignores read errors and serializes this cache. Keep a
        // valid numeric value, but /characteristics reports unavailable below.
        target = hap_char_get_val(s_targetTemp)->f;
        if (!std::isfinite(target)) target = metadata.min2[0] / 2.f;
        target = std::clamp(target, metadata.min2[0] / 2.f, metadata.max2[0] / 2.f);
    }
    const auto band = view.effectiveBand;
    install_temperature(s_targetTemp, metadata.min2[0], metadata.max2[0], metadata.step2[0], target);
    install_temperature(s_heatThresh, metadata.min2[1], metadata.max2[1], metadata.step2[1], band.heat2 / 2.f);
    install_temperature(s_coolThresh, metadata.min2[2], metadata.max2[2], metadata.step2[2], band.cool2 / 2.f);
    if (s_statusFault) {
        const bool equipment = cn105 && cn105->getState().hasError;
        hap_val_t fault{.u = static_cast<uint32_t>(!cn105 || !cn105->isHealthy() || equipment || failed || !s_targetReadable)};
        hap_char_update_val(s_statusFault, &fault);
    }
    homekit_record_temperature_metadata(metadata);
}

static int thermostat_read_cb(hap_char_t *hc, hap_status_t *status_code,
                              void *serv_priv, void *read_priv)
{
    if (g_homekitCtrl) publish_thermostat(*g_homekitCtrl);
    if (hc == s_targetTemp && !s_targetReadable) {
        *status_code = HAP_STATUS_COMM_ERR;
        return HAP_FAIL;
    }
    *status_code = HAP_STATUS_SUCCESS;
    return HAP_SUCCESS;
}

// ── Write callback ──────────────────────────────────────────────────────────

static int thermostat_write_cb(hap_write_data_t write_data[], int count,
                                void *serv_priv, void *write_priv)
{
    const bool healthy = g_homekitCtrl && g_homekitCtrl->isHealthy();
    SetpointControl::Command batch;
    bool invalid = false;
    for (int i = 0; i < count; ++i) {
        const auto& w = write_data[i];
        const char* uuid = hap_char_get_type_uuid(w.hc);
        if (!strcmp(uuid, HAP_CHAR_UUID_TARGET_HEATING_COOLING_STATE)) {
            if (w.val.u > 3) { invalid = true; continue; }
            batch.unit.power = w.val.u != 0;
            if (w.val.u) batch.unit.mode = hkTargetStateToCN105Mode(w.val.u);
        } else if (!strcmp(uuid, HAP_CHAR_UUID_TARGET_TEMPERATURE)) batch.unit.targetTemp = w.val.f;
        else if (!strcmp(uuid, HAP_CHAR_UUID_HEATING_THRESHOLD_TEMPERATURE)) batch.heatingThreshold = w.val.f;
        else if (!strcmp(uuid, HAP_CHAR_UUID_COOLING_THRESHOLD_TEMPERATURE)) batch.coolingThreshold = w.val.f;
    }
    const auto result = !healthy ? SetpointRange::Result::Unavailable : invalid ?
        SetpointRange::Result::Invalid : SetpointControl::submit(batch);
    if (result == SetpointRange::Result::Ok) install_temperature_metadata(g_homekitCtrl);
    int ret = HAP_SUCCESS;
    const auto acceptedBand = SetpointControl::settingsView().effectiveBand;
    if (result == SetpointRange::Result::Ok && (batch.heatingThreshold || batch.coolingThreshold)) {
        hap_val_t heat = {.f = acceptedBand.heat2 / 2.f};
        hap_val_t cool = {.f = acceptedBand.cool2 / 2.f};
        if (s_heatThresh) hap_char_update_val(s_heatThresh, &heat);
        if (s_coolThresh) hap_char_update_val(s_coolThresh, &cool);
    }
    for (int i = 0; i < count; ++i) {
        auto& w = write_data[i];
        const char* uuid = hap_char_get_type_uuid(w.hc);
        if (!strcmp(uuid, HAP_CHAR_UUID_CONFIGURED_NAME)) {
            hap_char_update_val(w.hc, &w.val); *w.status = HAP_STATUS_SUCCESS; continue;
        }
        if (!strcmp(uuid, HAP_CHAR_UUID_TEMPERATURE_DISPLAY_UNITS)) {
            const bool useF = w.val.u == 1;
            if (settings.get().useFahrenheit != useF) { settings.get().useFahrenheit = useF; settings.save(); }
            hap_char_update_val(w.hc, &w.val); *w.status = HAP_STATUS_SUCCESS; continue;
        }
        const bool climate = !strcmp(uuid, HAP_CHAR_UUID_TARGET_HEATING_COOLING_STATE) ||
            !strcmp(uuid, HAP_CHAR_UUID_TARGET_TEMPERATURE) ||
            !strcmp(uuid, HAP_CHAR_UUID_HEATING_THRESHOLD_TEMPERATURE) ||
            !strcmp(uuid, HAP_CHAR_UUID_COOLING_THRESHOLD_TEMPERATURE);
        if (!climate) { *w.status = HAP_STATUS_RES_ABSENT; continue; }
        if (result != SetpointRange::Result::Ok) {
            *w.status = healthy ? HAP_STATUS_VAL_INVALID : HAP_STATUS_RES_BUSY; ret = HAP_FAIL; continue;
        }
        hap_val_t value = w.val;
        if (!strcmp(uuid, HAP_CHAR_UUID_TARGET_TEMPERATURE)) value.f = g_homekitCtrl->getEffectiveState().targetTemp;
        if (!strcmp(uuid, HAP_CHAR_UUID_HEATING_THRESHOLD_TEMPERATURE)) value.f = acceptedBand.heat2 / 2.f;
        if (!strcmp(uuid, HAP_CHAR_UUID_COOLING_THRESHOLD_TEMPERATURE)) value.f = acceptedBand.cool2 / 2.f;
        hap_char_update_val(w.hc, &value); *w.status = HAP_STATUS_SUCCESS;
    }
    return ret;
}

// ── Service creation ────────────────────────────────────────────────────────

void homekit_create_thermostat(hap_acc_t *acc)
{
    // Create thermostat service with mandatory characteristics
    // (display units seeded from the persisted web-UI C/F setting)
    hap_serv_t *serv = hap_serv_thermostat_create(
        0, 0, 20.0f, 22.0f, settings.get().useFahrenheit ? 1 : 0);
    if (!serv) {
        LOG_ERROR("[HK:Thermo] Failed to create thermostat service");
        return;
    }

    // Get handles to mandatory chars
    s_currentState = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_CURRENT_HEATING_COOLING_STATE);
    s_targetState  = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_TARGET_HEATING_COOLING_STATE);
    s_currentTemp  = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_CURRENT_TEMPERATURE);
    s_targetTemp   = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_TARGET_TEMPERATURE);
    s_tempUnits    = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_TEMPERATURE_DISPLAY_UNITS);

    // Set valid values for target state (0=Off, 1=Heat, 2=Cool, 3=Auto),
    // filtered by the unit's capability mask (Settings > Unit Capabilities).
    uint8_t validStates[4];
    uint8_t nValid = 0;
    uint8_t mask = settings.get().modeMask;
    validStates[nValid++] = 0;
    if (mask & MODE_CAP_HEAT) validStates[nValid++] = 1;
    if (mask & MODE_CAP_COOL) validStates[nValid++] = 2;
    if (mask & MODE_CAP_AUTO) validStates[nValid++] = 3;
    hap_char_add_valid_vals(s_targetState, validStates, nValid);

    // Set constraints on current temperature
    hap_char_float_set_constraints(s_currentTemp, -10.0f, 50.0f, 0.5f);

    // Set constraints on target temperature
    hap_char_float_set_constraints(s_targetTemp, CN105_TEMP_MIN, CN105_TEMP_MAX, 0.5f);

    // Add optional characteristics
    s_statusFault = hap_char_status_fault_create(0);
    hap_serv_add_char(serv, s_statusFault);

    s_heatThresh = hap_char_heating_threshold_temperature_create(settings.get().heatingThreshold);
    hap_char_float_set_constraints(s_heatThresh, CN105_TEMP_MIN, CN105_TEMP_MAX, 0.5f);
    hap_serv_add_char(serv, s_heatThresh);

    s_coolThresh = hap_char_cooling_threshold_temperature_create(settings.get().coolingThreshold);
    hap_char_float_set_constraints(s_coolThresh, CN105_TEMP_MIN, CN105_TEMP_MAX, 0.5f);
    hap_serv_add_char(serv, s_coolThresh);

    s_statusActive = hap_char_status_active_create(true);
    hap_serv_add_char(serv, s_statusActive);

    // Name (default label) + ConfiguredName (rename)
    hap_serv_add_char(serv, hap_char_name_create(const_cast<char*>("Thermostat")));
    hap_char_t *cname = hap_char_string_create(
        const_cast<char*>(HAP_CHAR_UUID_CONFIGURED_NAME),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV,
        const_cast<char*>("Thermostat"));
    hap_serv_add_char(serv, cname);

    // Mark as primary service
    hap_serv_mark_primary(serv);

    // Register write callback
    hap_serv_set_write_cb(serv, thermostat_write_cb);
    hap_serv_set_read_cb(serv, thermostat_read_cb);
    install_temperature_metadata(g_homekitCtrl);

    // Add to accessory
    hap_acc_add_serv(acc, serv);

    LOG_INFO("[HK:Thermo] Service created (mask=0x%02X)", mask);
}

// ── Sync function (called from main loop) ───────────────────────────────────

static void publish_thermostat(CN105Controller& cn105)
{
    install_temperature_metadata(&cn105);
    if (!cn105.isHealthy()) {
        if (!s_wasDisconnected) {
            LOG_WARN("[HK:Thermo] CN105 unhealthy — setting StatusFault");
        }
        s_wasDisconnected = true;
        if (s_statusFault) {
            const hap_val_t *cur = hap_char_get_val(s_statusFault);
            if (cur && cur->u != 1) {
                hap_val_t v = { .u = 1 };
                hap_char_update_val(s_statusFault, &v);
            }
        }
        if (s_statusActive) {
            const hap_val_t *cur = hap_char_get_val(s_statusActive);
            if (cur && cur->b != false) {
                hap_val_t v; v.b = false;
                hap_char_update_val(s_statusActive, &v);
            }
        }
        return;
    }

    const CN105State s = cn105.getEffectiveState();

    bool forceSync = s_wasDisconnected;
    if (s_wasDisconnected) {
        LOG_INFO("[HK:Thermo] CN105 recovered — force syncing all characteristics");
        s_wasDisconnected = false;
    }

    // StatusActive = true
    if (s_statusActive) {
        const hap_val_t *cur = hap_char_get_val(s_statusActive);
        if (!cur || cur->b != true) {
            hap_val_t v; v.b = true;
            hap_char_update_val(s_statusActive, &v);
        }
    }

    // Current temperature — when the averaging blend is feeding the pump,
    // report its effective (offset-applied) value so Apple Home matches what
    // the pump is actually controlling on; otherwise the pump's own reported
    // room temp (which already echoes any single-source feed).
    if (s_currentTemp) {
        float room = s.roomTemp;
        RoomAvg::Status av = RoomAvg::status();
        if (av.feeding && !std::isnan(av.effective)) room = av.effective;
        const hap_val_t *cur = hap_char_get_val(s_currentTemp);
        if (forceSync || !cur || fabsf(cur->f - room) > 0.1f) {
            LOG_DEBUG("[HK:Thermo] sync currentTemp: %.1f C", room);
            hap_val_t v = { .f = room };
            hap_char_update_val(s_currentTemp, &v);
        }
    }

    // Target state
    if (s_targetState) {
        uint8_t hkTarget = cn105ToHKTargetState(s.power, s.mode);
        const hap_val_t *cur = hap_char_get_val(s_targetState);
        if (forceSync || !cur || cur->u != hkTarget) {
            LOG_DEBUG("[HK:Thermo] sync targetState: %s (%d)",
                      hkTargetStateStr(hkTarget), hkTarget);
            hap_val_t v = { .u = hkTarget };
            hap_char_update_val(s_targetState, &v);
        }
    }

    // Current state
    if (s_currentState) {
        uint8_t hkCurrent = deriveCurrentState(s);
        const hap_val_t *cur = hap_char_get_val(s_currentState);
        if (forceSync || !cur || cur->u != hkCurrent) {
            LOG_DEBUG("[HK:Thermo] sync currentState: %s (%d)",
                      hkCurrentStateStr(hkCurrent), hkCurrent);
            hap_val_t v = { .u = hkCurrent };
            hap_char_update_val(s_currentState, &v);
        }
    }

    // Display units follow the web UI's C/F setting (writes from the Home app
    // update the same setting — see thermostat_write_cb)
    if (s_tempUnits) {
        uint8_t units = settings.get().useFahrenheit ? 1 : 0;
        const hap_val_t *cur = hap_char_get_val(s_tempUnits);
        if (forceSync || !cur || cur->u != units) {
            hap_val_t v = { .u = units };
            hap_char_update_val(s_tempUnits, &v);
        }
    }
}

static void publish_queued(void* arg)
{
    if (!settings.isFactoryResetting()) publish_thermostat(*static_cast<CN105Controller*>(arg));
    s_publicationQueued.store(false);
}

void homekit_reconcile_temperature_metadata(CN105Controller& cn105)
{
    if (!homekit_is_started() || !s_targetTemp || settings.isFactoryResetting()) return;
    bool expected = false;
    if (!s_publicationQueued.compare_exchange_strong(expected, true)) return;
    httpd_handle_t* handle = hap_platform_httpd_get_handle();
    if (!handle || !*handle || httpd_queue_work(*handle, publish_queued, &cn105) != ESP_OK)
        s_publicationQueued.store(false); // Next sync retries the full publication.
}

void homekit_sync_thermostat(CN105Controller &cn105)
{
    uint32_t now = uptime_ms();
    if (now - s_lastSync < 2000) return;
    s_lastSync = now;
    // Keep steering on the main task, including before HAP starts. The caller
    // retains the existing safe-mode guard.
    // It recomputes current ownership and policy under the coordinator lock.
    if (cn105.isHealthy()) SetpointControl::steerAuto();
    homekit_reconcile_temperature_metadata(cn105);
}
