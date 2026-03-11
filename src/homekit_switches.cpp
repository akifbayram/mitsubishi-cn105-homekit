#include "homekit_services.h"

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
