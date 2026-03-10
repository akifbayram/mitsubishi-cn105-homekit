#include "homekit_services.h"
#include "settings.h"
#include <cmath>

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

// ════════════════════════════════════════════════════════════════════════════
// MitsubishiThermostat
// ════════════════════════════════════════════════════════════════════════════

MitsubishiThermostat::MitsubishiThermostat(CN105Controller *controller)
    : Service::Thermostat(), _ctrl(controller) {

    _currentState = new Characteristic::CurrentHeatingCoolingState(0);

    _targetState = new Characteristic::TargetHeatingCoolingState(0);
    _targetState->setValidValues(4, 0, 1, 2, 3);

    _currentTemp = new Characteristic::CurrentTemperature(20.0);
    _currentTemp->setRange(-10.0, 50.0, 0.5);

    _targetTemp = new Characteristic::TargetTemperature(22.0);
    _targetTemp->setRange(CN105_TEMP_MIN, CN105_TEMP_MAX, 0.5);

    _tempUnits = new Characteristic::TemperatureDisplayUnits(0);

    _statusFault = new Characteristic::StatusFault(0);

    // Dual setpoint for AUTO mode
    _heatThresh = new Characteristic::HeatingThresholdTemperature(settings.get().heatingThreshold);
    _heatThresh->setRange(CN105_TEMP_MIN, CN105_TEMP_MAX, 0.5);

    _coolThresh = new Characteristic::CoolingThresholdTemperature(settings.get().coolingThreshold);
    _coolThresh->setRange(CN105_TEMP_MIN, CN105_TEMP_MAX, 0.5);

    _statusActive = new Characteristic::StatusActive(1);

    LOG_INFO("[HK:Thermo] Service initialized (Off/Heat/Cool/Auto, 16-31°C)");
}

boolean MitsubishiThermostat::update() {
    if (!_ctrl->isHealthy()) {
        LOG_WARN("[HK:Thermo] update() REJECTED — CN105 not healthy");
        return false;
    }

    if (_targetState->updated()) {
        uint8_t hkState = _targetState->getNewVal();
        LOG_INFO("[HK:Thermo] HomeKit -> target state: %s (%d)",
                 hkTargetStateStr(hkState), hkState);
        if (hkState == 0) {
            _ctrl->setPower(false);
        } else {
            _ctrl->setPower(true);
            _ctrl->setMode(hkTargetStateToCN105Mode(hkState));
        }

    }

    if (_targetTemp->updated()) {
        float temp = _targetTemp->getNewVal<float>();
        LOG_INFO("[HK:Thermo] HomeKit -> target temp: %.1f°C", temp);
        _ctrl->setTargetTemp(temp);

    }

    if (_heatThresh->updated()) {
        float heat = _heatThresh->getNewVal<float>();
        float cool = _coolThresh->getVal<float>();
        // Enforce minimum 2°C gap
        if (cool - heat < 2.0f) {
            cool = heat + 2.0f;
            if (cool > CN105_TEMP_MAX) {
                cool = CN105_TEMP_MAX;
                heat = cool - 2.0f;
            }
            _coolThresh->setVal(cool);
            _heatThresh->setVal(heat);
        }
        settings.get().heatingThreshold = heat;
        settings.get().coolingThreshold = cool;
        settings.save();
        LOG_INFO("[HK:Thermo] HomeKit -> heating threshold: %.1f°C (cooling: %.1f°C)", heat, cool);
    }

    if (_coolThresh->updated()) {
        float cool = _coolThresh->getNewVal<float>();
        float heat = _heatThresh->getVal<float>();
        // Enforce minimum 2°C gap
        if (cool - heat < 2.0f) {
            heat = cool - 2.0f;
            if (heat < CN105_TEMP_MIN) {
                heat = CN105_TEMP_MIN;
                cool = heat + 2.0f;
            }
            _heatThresh->setVal(heat);
            _coolThresh->setVal(cool);
        }
        settings.get().heatingThreshold = heat;
        settings.get().coolingThreshold = cool;
        settings.save();
        LOG_INFO("[HK:Thermo] HomeKit -> cooling threshold: %.1f°C (heating: %.1f°C)", cool, heat);
    }

    return true;
}

void MitsubishiThermostat::loop() {
    uint32_t now = millis();
    if (now - _lastSync < 2000) return;
    _lastSync = now;

    if (!_ctrl->isHealthy()) {
        if (!_wasDisconnected) {
            LOG_WARN("[HK:Thermo] CN105 unhealthy — setting StatusFault");
        }
        _wasDisconnected = true;
        if (_statusFault->getVal() != 1) {
            _statusFault->setVal(1);
        }
        if (_statusActive->getVal() != 0) {
            _statusActive->setVal(0);
        }
        return;
    }

    // Use effective state (wanted values during grace, actual after confirmation)
    const CN105State s = _ctrl->getEffectiveState();

    bool forceSync = _wasDisconnected;
    if (_wasDisconnected) {
        LOG_INFO("[HK:Thermo] CN105 recovered — force syncing all characteristics");
        _wasDisconnected = false;
    }

    if (_statusActive->getVal() != 1) {
        _statusActive->setVal(1);
    }

    // StatusFault: 1 if HP reports error code, 0 if normal
    uint8_t fault = s.hasError ? 1 : 0;
    if (forceSync || _statusFault->getVal() != fault) {
        _statusFault->setVal(fault);
    }

    if (forceSync || fabsf(_currentTemp->getVal<float>() - s.roomTemp) > 0.1f) {
        LOG_DEBUG("[HK:Thermo] sync currentTemp: %.1f°C", s.roomTemp);
        _currentTemp->setVal(s.roomTemp);
    }

    if (forceSync || fabsf(_targetTemp->getVal<float>() - s.targetTemp) > 0.1f) {
        LOG_DEBUG("[HK:Thermo] sync targetTemp: %.1f°C", s.targetTemp);
        _targetTemp->setVal(s.targetTemp);
    }

    uint8_t hkTarget = cn105ToHKTargetState(s.power, s.mode);
    if (forceSync || _targetState->getVal() != hkTarget) {
        LOG_DEBUG("[HK:Thermo] sync targetState: %s (%d)",
                  hkTargetStateStr(hkTarget), hkTarget);
        _targetState->setVal(hkTarget);
    }

    uint8_t hkCurrent = deriveCurrentState(s);
    if (forceSync || _currentState->getVal() != hkCurrent) {
        LOG_DEBUG("[HK:Thermo] sync currentState: %s (%d)",
                  hkCurrentStateStr(hkCurrent), hkCurrent);
        _currentState->setVal(hkCurrent);
    }

    // ── Active-side tracking for dual setpoint AUTO ──────────────────────
    if (hkTarget == 3) {  // AUTO mode
        float autoTarget = resolveAutoTarget(s);
        if (fabsf(autoTarget - _lastSentAutoTarget) > 0.1f) {
            LOG_INFO("[HK:Thermo] AUTO active-side target: %.1f°C (autoSub=%d)",
                     autoTarget, s.autoSubMode);
            _ctrl->setTargetTemp(autoTarget);
            _ctrl->sendPendingChanges();
            _lastSentAutoTarget = autoTarget;
        }
    }

    // Sync threshold values from NVS (may be changed by WebUI)
    float heat = settings.get().heatingThreshold;
    float cool = settings.get().coolingThreshold;
    if (forceSync || fabsf(_heatThresh->getVal<float>() - heat) > 0.1f) {
        _heatThresh->setVal(heat);
    }
    if (forceSync || fabsf(_coolThresh->getVal<float>() - cool) > 0.1f) {
        _coolThresh->setVal(cool);
    }
}

uint8_t MitsubishiThermostat::cn105ToHKTargetState(bool power, uint8_t mode) {
    if (!power) return 0;
    switch (mode) {
        case CN105_MODE_HEAT: return 1;
        case CN105_MODE_COOL: return 2;
        case CN105_MODE_AUTO: return 3;
        default:              return 0;
    }
}

uint8_t MitsubishiThermostat::hkTargetStateToCN105Mode(uint8_t hkState) {
    switch (hkState) {
        case 1:  return CN105_MODE_HEAT;
        case 2:  return CN105_MODE_COOL;
        case 3:  return CN105_MODE_AUTO;
        default: return CN105_MODE_AUTO;
    }
}

uint8_t MitsubishiThermostat::deriveCurrentState(const CN105State &s) {
    if (!s.power) return 0;

    if (s.operating) {
        switch (s.mode) {
            case CN105_MODE_HEAT: return 1;
            case CN105_MODE_COOL: return 2;
            case CN105_MODE_AUTO:
                // Use autoSubMode from 0x09 for definitive answer
                if (s.autoSubMode == 0x02) return 1;  // AUTO_HEAT
                if (s.autoSubMode == 0x01) return 2;  // AUTO_COOL
                // Fallback to temp comparison
                if (s.roomTemp < s.targetTemp - 0.5f) return 1;
                if (s.roomTemp > s.targetTemp + 0.5f) return 2;
                return 1;
            default: return 0;
        }
    }

    return 0;
}

float MitsubishiThermostat::resolveAutoTarget(const CN105State &s) const {
    float heat = settings.get().heatingThreshold;
    float cool = settings.get().coolingThreshold;
    float mid = (heat + cool) / 2.0f;
    // Round midpoint to nearest 0.5°C
    mid = round(mid * 2.0f) / 2.0f;

    switch (s.autoSubMode) {
        case 0x02:  // AUTO_HEAT — unit is heating
            return heat;
        case 0x01:  // AUTO_COOL — unit is cooling
            return cool;
        case 0x00:  // AUTO_OFF
        case 0x03:  // AUTO_LEADER
        default:
            return mid;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// MitsubishiFan
// ════════════════════════════════════════════════════════════════════════════

MitsubishiFan::MitsubishiFan(CN105Controller *controller)
    : Service::Fan(), _ctrl(controller) {

    _active = new Characteristic::Active(1);

    _speed = new Characteristic::RotationSpeed(0);
    _speed->setRange(0, 100, 1);

    _statusActive = new Characteristic::StatusActive(1);

    LOG_INFO("[HK:Fan] Service initialized (0-100%%, bands: Quiet/1/2/3/4)");
}

boolean MitsubishiFan::update() {
    if (!_ctrl->isHealthy()) {
        LOG_WARN("[HK:Fan] update() REJECTED — CN105 not healthy");
        return false;
    }

    const CN105State s = _ctrl->getEffectiveState();

    if (_active->updated()) {
        uint8_t active = _active->getNewVal();
        LOG_INFO("[HK:Fan] HomeKit -> active: %d", active);
        if (active == 0) {
            _ctrl->setFanSpeed(CN105_FAN_AUTO);
        }

    }

    if (_speed->updated() && s.power) {
        uint8_t pct = _speed->getNewVal();
        uint8_t fanByte = percentToCN105Fan(pct);
        LOG_INFO("[HK:Fan] HomeKit -> speed: %d%% -> CN105 fan=0x%02X", pct, fanByte);
        _ctrl->setFanSpeed(fanByte);

    } else if (_speed->updated() && !s.power) {
        LOG_WARN("[HK:Fan] HomeKit -> speed: %d%% IGNORED (unit off)", _speed->getNewVal());
    }

    return true;
}

void MitsubishiFan::loop() {
    uint32_t now = millis();
    if (now - _lastSync < 2000) return;
    _lastSync = now;

    if (!_ctrl->isHealthy()) {
        _wasDisconnected = true;
        if (_statusActive->getVal() != 0) {
            _statusActive->setVal(0);
        }
        return;
    }

    const CN105State s = _ctrl->getEffectiveState();

    bool forceSync = _wasDisconnected;
    if (_wasDisconnected) {
        LOG_INFO("[HK:Fan] CN105 recovered — force syncing");
        _wasDisconnected = false;
    }

    if (_statusActive->getVal() != 1) {
        _statusActive->setVal(1);
    }

    uint8_t active = s.power ? 1 : 0;
    if (forceSync || _active->getVal() != active) {
        LOG_DEBUG("[HK:Fan] sync active: %d", active);
        _active->setVal(active);
    }

    if (s.power) {
        uint8_t pct = cn105FanToPercent(s.fanSpeed);
        if (forceSync || _speed->getVal() != pct) {
            LOG_DEBUG("[HK:Fan] sync speed: %d%%", pct);
            _speed->setVal(pct);
        }
    }
}

uint8_t MitsubishiFan::cn105FanToPercent(uint8_t fan) {
    switch (fan) {
        case CN105_FAN_AUTO:  return 0;
        case CN105_FAN_QUIET: return 10;
        case CN105_FAN_1:     return 27;
        case CN105_FAN_2:     return 44;
        case CN105_FAN_3:     return 61;
        case CN105_FAN_4:     return 78;
        default:              return 0;
    }
}

uint8_t MitsubishiFan::percentToCN105Fan(uint8_t pct) {
    if (pct == 0)       return CN105_FAN_AUTO;
    if (pct <= 17)      return CN105_FAN_QUIET;
    if (pct <= 34)      return CN105_FAN_1;
    if (pct <= 51)      return CN105_FAN_2;
    if (pct <= 68)      return CN105_FAN_3;
    return CN105_FAN_4;
}

// ════════════════════════════════════════════════════════════════════════════
// MitsubishiFanAutoSwitch
// ════════════════════════════════════════════════════════════════════════════

MitsubishiFanAutoSwitch::MitsubishiFanAutoSwitch(CN105Controller *controller)
    : Service::Switch(), _ctrl(controller) {

    _on = new Characteristic::On(true);
    new Characteristic::ConfiguredName("Fan Auto");

    _statusActive = new Characteristic::StatusActive(1);

    LOG_INFO("[HK:FanAuto] Service initialized (toggle for auto fan speed)");
}

boolean MitsubishiFanAutoSwitch::update() {
    if (!_ctrl->isHealthy()) {
        LOG_WARN("[HK:FanAuto] update() REJECTED — CN105 not healthy");
        return false;
    }

    if (_on->updated()) {
        const CN105State s = _ctrl->getEffectiveState();
        if (!s.power) {
            LOG_WARN("[HK:FanAuto] HomeKit -> toggle IGNORED (unit off)");
            return true;
        }

        bool autoMode = _on->getNewVal();
        LOG_INFO("[HK:FanAuto] HomeKit -> auto: %s", autoMode ? "ON" : "OFF");
        if (autoMode) {
            _ctrl->setFanSpeed(CN105_FAN_AUTO);
        } else {
            _ctrl->setFanSpeed(CN105_FAN_2);
        }

    }
    return true;
}

void MitsubishiFanAutoSwitch::loop() {
    uint32_t now = millis();
    if (now - _lastSync < 2000) return;
    _lastSync = now;

    if (!_ctrl->isHealthy()) {
        _wasDisconnected = true;
        if (_statusActive->getVal() != 0) {
            _statusActive->setVal(0);
        }
        return;
    }

    const CN105State s = _ctrl->getEffectiveState();

    bool forceSync = _wasDisconnected;
    if (_wasDisconnected) {
        LOG_INFO("[HK:FanAuto] CN105 recovered — force syncing");
        _wasDisconnected = false;
    }

    if (_statusActive->getVal() != 1) {
        _statusActive->setVal(1);
    }

    bool isAuto = (s.fanSpeed == CN105_FAN_AUTO);
    if (forceSync || _on->getVal() != isAuto) {
        LOG_DEBUG("[HK:FanAuto] sync auto: %s", isAuto ? "ON" : "OFF");
        _on->setVal(isAuto);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// MitsubishiFanModeSwitch
// ════════════════════════════════════════════════════════════════════════════

MitsubishiFanModeSwitch::MitsubishiFanModeSwitch(CN105Controller *controller)
    : Service::Switch(), _ctrl(controller) {

    _on = new Characteristic::On(false);
    new Characteristic::ConfiguredName("Fan Mode");
    _statusActive = new Characteristic::StatusActive(1);

    LOG_INFO("[HK:FanMode] Service initialized (FAN mode switch)");
}

boolean MitsubishiFanModeSwitch::update() {
    if (!_ctrl->isHealthy()) {
        LOG_WARN("[HK:FanMode] update() REJECTED — CN105 not healthy");
        return false;
    }

    if (_on->updated()) {
        bool on = _on->getNewVal();
        if (on) {
            LOG_INFO("[HK:FanMode] HomeKit -> ON");
            _ctrl->setPower(true);
            _ctrl->setMode(CN105_MODE_FAN);
        } else {
            LOG_INFO("[HK:FanMode] HomeKit -> OFF (powering off)");
            _ctrl->setPower(false);
        }
    }

    return true;
}

void MitsubishiFanModeSwitch::loop() {
    uint32_t now = millis();
    if (now - _lastSync < 2000) return;
    _lastSync = now;

    if (!_ctrl->isHealthy()) {
        _wasDisconnected = true;
        if (_statusActive->getVal() != 0) {
            _statusActive->setVal(0);
        }
        return;
    }

    const CN105State s = _ctrl->getEffectiveState();

    bool forceSync = _wasDisconnected;
    if (_wasDisconnected) {
        LOG_INFO("[HK:FanMode] CN105 recovered — force syncing");
        _wasDisconnected = false;
    }

    if (_statusActive->getVal() != 1) {
        _statusActive->setVal(1);
    }

    bool isFanMode = (s.power && s.mode == CN105_MODE_FAN);
    if (forceSync || _on->getVal() != isFanMode) {
        LOG_DEBUG("[HK:FanMode] sync: %s", isFanMode ? "ON" : "OFF");
        _on->setVal(isFanMode);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// MitsubishiDryModeSwitch
// ════════════════════════════════════════════════════════════════════════════

MitsubishiDryModeSwitch::MitsubishiDryModeSwitch(CN105Controller *controller)
    : Service::Switch(), _ctrl(controller) {

    _on = new Characteristic::On(false);
    new Characteristic::ConfiguredName("Dry Mode");
    _statusActive = new Characteristic::StatusActive(1);

    LOG_INFO("[HK:DryMode] Service initialized (DRY mode switch)");
}

boolean MitsubishiDryModeSwitch::update() {
    if (!_ctrl->isHealthy()) {
        LOG_WARN("[HK:DryMode] update() REJECTED — CN105 not healthy");
        return false;
    }

    if (_on->updated()) {
        bool on = _on->getNewVal();
        if (on) {
            LOG_INFO("[HK:DryMode] HomeKit -> ON");
            _ctrl->setPower(true);
            _ctrl->setMode(CN105_MODE_DRY);
        } else {
            LOG_INFO("[HK:DryMode] HomeKit -> OFF (powering off)");
            _ctrl->setPower(false);
        }
    }

    return true;
}

void MitsubishiDryModeSwitch::loop() {
    uint32_t now = millis();
    if (now - _lastSync < 2000) return;
    _lastSync = now;

    if (!_ctrl->isHealthy()) {
        _wasDisconnected = true;
        if (_statusActive->getVal() != 0) {
            _statusActive->setVal(0);
        }
        return;
    }

    const CN105State s = _ctrl->getEffectiveState();

    bool forceSync = _wasDisconnected;
    if (_wasDisconnected) {
        LOG_INFO("[HK:DryMode] CN105 recovered — force syncing");
        _wasDisconnected = false;
    }

    if (_statusActive->getVal() != 1) {
        _statusActive->setVal(1);
    }

    bool isDryMode = (s.power && s.mode == CN105_MODE_DRY);
    if (forceSync || _on->getVal() != isDryMode) {
        LOG_DEBUG("[HK:DryMode] sync: %s", isDryMode ? "ON" : "OFF");
        _on->setVal(isDryMode);
    }
}
