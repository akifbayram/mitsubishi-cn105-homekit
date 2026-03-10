#pragma once

#include <HomeSpan.h>
#include "cn105_protocol.h"

// ── HomeKit Thermostat Service ──────────────────────────────────────────────
//
// Maps Apple HAP Thermostat characteristics to CN105 commands.
//
// HomeKit Heating/Cooling modes:
//   0 = Off, 1 = Heat, 2 = Cool, 3 = Auto
//
// HomeKit Current State:
//   0 = Off/Idle, 1 = Heating, 2 = Cooling
//
// Fan speed is exposed via a separate Fan service with RotationSpeed.
// ────────────────────────────────────────────────────────────────────────────

class MitsubishiThermostat : public Service::Thermostat {
public:
    MitsubishiThermostat(CN105Controller *controller);

    /// Called by HomeSpan when the user changes a value
    boolean update() override;

    /// Called by HomeSpan periodically — we push CN105 state to HomeKit
    void loop() override;

private:
    CN105Controller *_ctrl;

    SpanCharacteristic *_currentState;
    SpanCharacteristic *_targetState;
    SpanCharacteristic *_currentTemp;
    SpanCharacteristic *_targetTemp;
    SpanCharacteristic *_tempUnits;
    SpanCharacteristic *_statusFault;
    SpanCharacteristic *_heatThresh;
    SpanCharacteristic *_coolThresh;
    SpanCharacteristic *_statusActive;

    uint32_t _lastSync = 0;
    bool     _wasDisconnected = true;  // Force full sync on first connect & recovery
    float    _lastSentAutoTarget = 0.0f;  // Track last target sent in AUTO to avoid redundant SETs

    /// Map CN105 mode + power to HomeKit target state
    uint8_t cn105ToHKTargetState(bool power, uint8_t mode);

    /// Map HomeKit target state to CN105 mode
    uint8_t hkTargetStateToCN105Mode(uint8_t hkState);

    /// Determine HomeKit current heating/cooling state from CN105 state
    uint8_t deriveCurrentState(const CN105State &s);

    /// Resolve which target temperature to send in AUTO mode based on autoSubMode
    float resolveAutoTarget(const CN105State &s) const;
};

// ── HomeKit Fan Service ─────────────────────────────────────────────────────
//
// Fan speed percentage bands → Mitsubishi discrete levels:
//   0%        = Auto (fan speed controlled automatically by unit)
//   1-17%     = Quiet (lowest, near-silent operation)
//   18-34%    = Speed 1
//   35-51%    = Speed 2
//   52-68%    = Speed 3
//   69-100%   = Speed 4 (highest speed)
//
// Fan state is only relevant when the unit is running:
//   - Active=1 when power is on, Active=0 when power is off
//   - Speed updates are only sent/synced while the unit is running
//   - Setting Active or Speed while the unit is off is a no-op
//
// When the fan Active characteristic is set to Inactive (0),
// fan speed is set to Auto mode on the Mitsubishi unit.
// ────────────────────────────────────────────────────────────────────────────

class MitsubishiFan : public Service::Fan {
public:
    MitsubishiFan(CN105Controller *controller);

    boolean update() override;
    void loop() override;

private:
    CN105Controller *_ctrl;

    SpanCharacteristic *_active;
    SpanCharacteristic *_speed;
    SpanCharacteristic *_statusActive;

    uint32_t _lastSync = 0;
    bool     _wasDisconnected = true;  // Force full sync on first connect & recovery

    /// Map CN105 fan byte to percentage
    uint8_t cn105FanToPercent(uint8_t fan);

    /// Map percentage to CN105 fan byte
    uint8_t percentToCN105Fan(uint8_t pct);
};

// ── HomeKit Fan Auto Switch Service ────────────────────────────────────────
//
// Exposes Auto fan speed as a separate toggle (Switch) in HomeKit.
//
// When ON:  Fan speed is set to Auto (CN105 decides speed)
// When OFF: Fan uses the manual speed from the Fan service's RotationSpeed
//
// This switch reflects the current state — if the CN105 reports fan=Auto,
// the switch shows ON; if a manual speed is reported, it shows OFF.
//
// Like the Fan service, this switch only acts when the unit is running.
// Toggling while the unit is off is a no-op.
// ────────────────────────────────────────────────────────────────────────────

class MitsubishiFanAutoSwitch : public Service::Switch {
public:
    MitsubishiFanAutoSwitch(CN105Controller *controller);

    boolean update() override;
    void loop() override;

private:
    CN105Controller *_ctrl;

    SpanCharacteristic *_on;
    SpanCharacteristic *_statusActive;

    uint32_t _lastSync = 0;
    bool     _wasDisconnected = true;  // Force full sync on first connect & recovery
};

// ── HomeKit FAN Mode Switch Service ───────────────────────────────────────
//
// Exposes FAN mode as a HomeKit switch, since HomeKit's Thermostat only
// supports Heat/Cool/Auto/Off.
//
// ON:  Sets mode=FAN + power=ON
// OFF: Powers off the unit
//
// Mutual exclusion with DRY mode: handled naturally — if mode changes
// externally, loop() sees mode≠FAN and reports On=false.
// ────────────────────────────────────────────────────────────────────────────

class MitsubishiFanModeSwitch : public Service::Switch {
public:
    MitsubishiFanModeSwitch(CN105Controller *controller);

    boolean update() override;
    void loop() override;

private:
    CN105Controller *_ctrl;

    SpanCharacteristic *_on;
    SpanCharacteristic *_statusActive;

    uint32_t _lastSync = 0;
    bool     _wasDisconnected = true;
};

// ── HomeKit DRY Mode Switch Service ───────────────────────────────────────
//
// Same pattern as FanModeSwitch but for DRY (dehumidify) mode.
//
// ON:  Sets mode=DRY + power=ON
// OFF: Powers off the unit
// ────────────────────────────────────────────────────────────────────────────

class MitsubishiDryModeSwitch : public Service::Switch {
public:
    MitsubishiDryModeSwitch(CN105Controller *controller);

    boolean update() override;
    void loop() override;

private:
    CN105Controller *_ctrl;

    SpanCharacteristic *_on;
    SpanCharacteristic *_statusActive;

    uint32_t _lastSync = 0;
    bool     _wasDisconnected = true;
};
