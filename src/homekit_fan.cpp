#include "homekit_services.h"

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
            _ctrl->setPower(false);
        }

    }

    if (_speed->updated()) {
        uint8_t pct = _speed->getNewVal();
        if (pct == 0) {
            LOG_INFO("[HK:Fan] HomeKit -> speed: 0%% -> power off");
            _ctrl->setPower(false);
        } else if (s.power) {
            uint8_t fanByte = percentToCN105Fan(pct);
            LOG_INFO("[HK:Fan] HomeKit -> speed: %d%% -> CN105 fan=0x%02X", pct, fanByte);
            _ctrl->setFanSpeed(fanByte);
        } else {
            LOG_WARN("[HK:Fan] HomeKit -> speed: %d%% IGNORED (unit off)", pct);
        }
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
        case CN105_FAN_1:     return 30;
        case CN105_FAN_2:     return 50;
        case CN105_FAN_3:     return 70;
        case CN105_FAN_4:     return 100;
        default:              return 0;
    }
}

uint8_t MitsubishiFan::percentToCN105Fan(uint8_t pct) {
    if (pct <= 20)      return CN105_FAN_QUIET;
    if (pct <= 40)      return CN105_FAN_1;
    if (pct <= 60)      return CN105_FAN_2;
    if (pct <= 80)      return CN105_FAN_3;
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
