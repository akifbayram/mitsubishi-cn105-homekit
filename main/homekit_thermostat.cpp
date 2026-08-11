#include "homekit_services.h"
#include "cn105_strings.h"
#include "room_avg.h"
#include "settings.h"
#include "logging.h"
#include "esp_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <hap.h>
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
                    float bandMid = (settings.get().heatingThreshold +
                                     settings.get().coolingThreshold) * 0.5f;
                    return (s.roomTemp <= bandMid) ? 1 : 2;
                }
            default: return 0;
        }
    }

    return 0;
}

// Map the two HomeKit thresholds onto the unit's single AUTO setpoint using a
// room-vs-band deadband (the approach used by echavet's HEAT_COOL handling):
//   - room below the heating threshold -> drive heat to the heating threshold
//   - room above the cooling threshold -> drive cool to the cooling threshold
//   - room inside the band             -> feed the unit its own room temp so it idles
// The unit's reported autoSubMode is intentionally NOT used to pick the side: it
// lags and can latch to a stale side (e.g. left on HEAT after the room warmed past
// `cool`), which made AUTO send the heating threshold while the room was already hot.
//
// resendDeadband is how far the unit's reported setpoint may drift from the
// resolved target before the sync loop corrects it. The comparison runs on
// cn105.quantizeSetpoint(setpoint) — the value the unit will actually report
// back — so quantization can never look like drift and the band edges use a
// tight sub-grid 0.25 C. Inside the band the setpoint merely shadows the
// room to keep the unit idle, so a coarse 1.0 C deadband avoids re-sending
// a SET for every 0.5 C step of the room sensor.
struct AutoTarget {
    float setpoint;
    float resendDeadband;
};

static AutoTarget resolveAutoTarget(const CN105State &s) {
    float heat = settings.get().heatingThreshold;
    float cool = settings.get().coolingThreshold;
    float room = s.roomTemp;

    if (room < heat) return {heat, 0.25f};  // below band -> heat toward heating threshold
    if (room > cool) return {cool, 0.25f};  // above band -> cool toward cooling threshold
    return {room, 1.0f};                    // inside band -> setpoint == room -> unit idles
}

// ── Write callback ──────────────────────────────────────────────────────────

static int thermostat_write_cb(hap_write_data_t write_data[], int count,
                                void *serv_priv, void *write_priv)
{
    bool healthy = g_homekitCtrl && g_homekitCtrl->isHealthy();

    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; i++) {
        hap_write_data_t *w = &write_data[i];
        const char *uuid = hap_char_get_type_uuid(w->hc);

        // ConfiguredName (Home app rename) doesn't touch the heat pump —
        // accept it even when CN105 is down.
        if (!strcmp(uuid, HAP_CHAR_UUID_CONFIGURED_NAME)) {
            hap_char_update_val(w->hc, &w->val);
            *(w->status) = HAP_STATUS_SUCCESS;
            continue;
        }

        // Display units don't touch the heat pump either. Persisted to the
        // same setting the web UI's C/F toggle uses, so both stay in step
        // (the sync loop pushes web-side changes back to this char).
        if (!strcmp(uuid, HAP_CHAR_UUID_TEMPERATURE_DISPLAY_UNITS)) {
            bool useF = (w->val.u == 1);
            if (settings.get().useFahrenheit != useF) {
                settings.get().useFahrenheit = useF;
                settings.save();
                LOG_INFO("[HK:Thermo] HomeKit -> display units: %s", useF ? "F" : "C");
            }
            hap_char_update_val(w->hc, &w->val);
            *(w->status) = HAP_STATUS_SUCCESS;
            continue;
        }

        if (!healthy) {
            LOG_WARN("[HK:Thermo] write REJECTED — CN105 not healthy");
            *(w->status) = HAP_STATUS_RES_BUSY;
            ret = HAP_FAIL;
            continue;
        }

        if (!strcmp(uuid, HAP_CHAR_UUID_TARGET_HEATING_COOLING_STATE)) {
            uint8_t hkState = w->val.u;
            // Off (0) is never gated; 1..3 reuse the existing HK->CN105 table
            // so the gating check can't drift from what setMode() will do.
            uint8_t capBit = (hkState == 0)
                ? 0 : modeToCapBit(hkTargetStateToCN105Mode(hkState));
            if (capBit && !(settings.get().modeMask & capBit)) {
                // Valid-values is advisory to controllers; also covers the
                // window where the mask changed but the device hasn't rebooted.
                LOG_WARN("[HK:Thermo] target state %d rejected — disabled by capability mask", hkState);
                *(w->status) = HAP_STATUS_VAL_INVALID;
                ret = HAP_FAIL;
                continue;
            }
            LOG_INFO("[HK:Thermo] HomeKit -> target state: %s (%d)",
                     hkTargetStateStr(hkState), hkState);
            if (hkState == 0) {
                g_homekitCtrl->setPower(false);
            } else {
                g_homekitCtrl->setPower(true);
                g_homekitCtrl->setMode(hkTargetStateToCN105Mode(hkState));
            }
            hap_char_update_val(w->hc, &w->val);
            *(w->status) = HAP_STATUS_SUCCESS;

        } else if (!strcmp(uuid, HAP_CHAR_UUID_TARGET_TEMPERATURE)) {
            float temp = w->val.f;
            LOG_INFO("[HK:Thermo] HomeKit -> target temp: %.1f C", temp);
            g_homekitCtrl->setTargetTemp(temp);
            hap_char_update_val(w->hc, &w->val);
            *(w->status) = HAP_STATUS_SUCCESS;

        } else if (!strcmp(uuid, HAP_CHAR_UUID_HEATING_THRESHOLD_TEMPERATURE)) {
            float heat = w->val.f;
            float cool = settings.get().coolingThreshold;
            // Enforce minimum 2 deg C gap
            if (cool - heat < 2.0f) {
                cool = heat + 2.0f;
                if (cool > CN105_TEMP_MAX) {
                    cool = CN105_TEMP_MAX;
                    heat = cool - 2.0f;
                }
                hap_val_t cv = { .f = cool };
                hap_char_update_val(s_coolThresh, &cv);
            }
            hap_val_t hv = { .f = heat };
            hap_char_update_val(w->hc, &hv);
            settings.get().heatingThreshold = heat;
            settings.get().coolingThreshold = cool;
            settings.save();
            LOG_INFO("[HK:Thermo] HomeKit -> heating threshold: %.1f C (cooling: %.1f C)", heat, cool);
            *(w->status) = HAP_STATUS_SUCCESS;

        } else if (!strcmp(uuid, HAP_CHAR_UUID_COOLING_THRESHOLD_TEMPERATURE)) {
            float cool = w->val.f;
            float heat = settings.get().heatingThreshold;
            // Enforce minimum 2 deg C gap
            if (cool - heat < 2.0f) {
                heat = cool - 2.0f;
                if (heat < CN105_TEMP_MIN) {
                    heat = CN105_TEMP_MIN;
                    cool = heat + 2.0f;
                }
                hap_val_t hv = { .f = heat };
                hap_char_update_val(s_heatThresh, &hv);
            }
            hap_val_t cv = { .f = cool };
            hap_char_update_val(w->hc, &cv);
            settings.get().heatingThreshold = heat;
            settings.get().coolingThreshold = cool;
            settings.save();
            LOG_INFO("[HK:Thermo] HomeKit -> cooling threshold: %.1f C (heating: %.1f C)", cool, heat);
            *(w->status) = HAP_STATUS_SUCCESS;

        } else {
            *(w->status) = HAP_STATUS_RES_ABSENT;
        }
    }

    if (healthy) {
        g_homekitCtrl->sendPendingChanges();
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

    // Add to accessory
    hap_acc_add_serv(acc, serv);

    LOG_INFO("[HK:Thermo] Service created (mask=0x%02X, %.0f-%.0f C)",
             mask, CN105_TEMP_MIN, CN105_TEMP_MAX);
}

// ── Sync function (called from main loop) ───────────────────────────────────

void homekit_sync_thermostat(CN105Controller &cn105)
{
    uint32_t now = uptime_ms();
    if (now - s_lastSync < 2000) return;
    s_lastSync = now;

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

    // StatusFault
    if (s_statusFault) {
        uint32_t fault = s.hasError ? 1 : 0;
        const hap_val_t *cur = hap_char_get_val(s_statusFault);
        if (forceSync || !cur || cur->u != fault) {
            hap_val_t v = { .u = fault };
            hap_char_update_val(s_statusFault, &v);
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

    // Target temperature
    if (s_targetTemp) {
        // The heat pump can report a setpoint outside the char constraints
        // (e.g. 31.0 set from the IR remote; char max is 30.5), and
        // hap_char_update_val rejects out-of-range values outright, freezing
        // the Home-app display — clamp before syncing.
        float target = std::clamp(s.targetTemp, CN105_TEMP_MIN, CN105_TEMP_MAX);
        const hap_val_t *cur = hap_char_get_val(s_targetTemp);
        if (forceSync || !cur || fabsf(cur->f - target) > 0.1f) {
            LOG_DEBUG("[HK:Thermo] sync targetTemp: %.1f C", target);
            hap_val_t v = { .f = target };
            hap_char_update_val(s_targetTemp, &v);
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

    // Dual-setpoint AUTO: drive the unit's single setpoint from the two
    // HomeKit thresholds. The resolved target is compared against the
    // *effective* setpoint (wanted value during the grace window, heat-pump-
    // confirmed after), so a lost SET, an IR-remote change, or a setpoint
    // left over from another mode all self-correct on a later pass — there
    // is deliberately no "last sent" shadow state to go stale.
    if (s.power && s.mode == CN105_MODE_AUTO) {
        AutoTarget want = resolveAutoTarget(s);
        if (fabsf(cn105.quantizeSetpoint(want.setpoint) - s.targetTemp) > want.resendDeadband) {
            LOG_INFO("[HK:Thermo] AUTO target %.1fC (unit at %.1f, room %.1f, heat %.1f, cool %.1f)",
                     want.setpoint, s.targetTemp, s.roomTemp,
                     settings.get().heatingThreshold, settings.get().coolingThreshold);
            cn105.setTargetTemp(want.setpoint);
            cn105.sendPendingChanges();
        }
    }

    // Sync threshold values from NVS (may be changed by WebUI)
    float heat = settings.get().heatingThreshold;
    float cool = settings.get().coolingThreshold;
    if (s_heatThresh) {
        const hap_val_t *cur = hap_char_get_val(s_heatThresh);
        if (forceSync || !cur || fabsf(cur->f - heat) > 0.1f) {
            hap_val_t v = { .f = heat };
            hap_char_update_val(s_heatThresh, &v);
        }
    }
    if (s_coolThresh) {
        const hap_val_t *cur = hap_char_get_val(s_coolThresh);
        if (forceSync || !cur || fabsf(cur->f - cool) > 0.1f) {
            hap_val_t v = { .f = cool };
            hap_char_update_val(s_coolThresh, &v);
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
