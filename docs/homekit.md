# HomeKit Details

Apple's HomeKit Thermostat service only supports Heat, Cool, Auto, and Off. This document covers the mappings and workarounds for features that don't fit natively.

## Thermostat

| HomeKit Mode | CN105 Mode | Behavior |
|-------------|------------|----------|
| Off | Power Off | Unit off |
| Heat | 0x01 HEAT | Heat to target temperature |
| Cool | 0x03 COOL | Cool to target temperature |
| Auto | 0x08 AUTO | Dual setpoint — heats below heating threshold, cools above cooling threshold |

## FAN & DRY Mode Switches

The Thermostat service has no representation for **FAN** (circulation-only) or **DRY** (dehumidification) modes. These are exposed as separate switches — turning one on powers the unit in that mode, turning it off powers the unit off. Only one mode can be active at a time; switching modes via the thermostat automatically reflects in the switches.

## Fan Speed

HomeKit's Fan service uses a 0–100% rotation speed slider, mapped to discrete speed levels:

| HomeKit % | Speed |
|-----------|-------|
| 0% | Off (powers off unit) |
| 1–20% | Quiet |
| 21–40% | Speed 1 |
| 41–60% | Speed 2 |
| 61–80% | Speed 3 |
| 81–100% | Speed 4 |

Setting the slider to 0% or deactivating the fan service powers the unit off. Auto fan speed is controlled exclusively via the **Fan Auto** switch.

## Dual Setpoint (Auto Mode)

The HomeKit Thermostat service supports independent heating and cooling thresholds, but the CN105 protocol only accepts a **single target temperature**. The controller maps the two thresholds onto that single setpoint with a room-vs-band deadband: room below the heating threshold → send the heating threshold; room above the cooling threshold → send the cooling threshold; room inside the band → send the current room temperature so the unit idles. The heat pump's reported `autoSubMode` is used only for status display, not to choose the setpoint (it lags and can latch to a stale side). A 2°C minimum gap is enforced between thresholds.

## Vane Control

Vane positions (including swing mode) are controlled exclusively through the [web UI](../README.md#web-ui).

## Temperature

The heat pump supports 16–31°C. The web UI offers a °C/°F display toggle, but the protocol always operates in Celsius. Half-degree precision is supported when the unit's enhanced temperature encoding is detected.

### Known issue — °F values differ between Apple Home and the web UI

Both interfaces read the same stored Celsius value but convert to Fahrenheit differently. The web UI uses Mitsubishi's native (non-linear) °F mapping, which matches the °F the heat pump itself displays; Apple Home applies a fixed linear °C→°F conversion that cannot be customized. They diverge by up to 1°F at some setpoints — e.g. 22°C reads **71°F** in the web UI but **72°F** in Home (also around 18–19°C and 28–30.5°C); values such as 20, 23, 24, and 25°C agree. HomeKit, the web UI, and the unit's own panel cannot all match because Apple is hard-wired to linear and the unit uses the table. Left as-is intentionally so the web UI stays accurate to the unit.

## Web UI–Only Diagnostics

The following data is available from the heat pump but not exposed as HomeKit services:

| Sensor | Source |
|--------|--------|
| Compressor frequency (Hz) | 0x06 status response |
| Outside air temperature | 0x03 temp response |
| Runtime hours | 0x03 temp response |
| Error code | 0x04 error response |
| Sub mode (Normal/Defrost/Preheat/Standby) | 0x09 standby response |
| Operating stage (Idle/Diffuse) | 0x09 standby response |
