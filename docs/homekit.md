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

**Who owns the Auto setpoint.** A Mitsubishi wall controller (MA bus — PAR-xxMAA, MHK) or an IR remote writes the same setpoint register, so the two sides take turns (`main/auto_band.h`, persisted as `autoOwnerUnit`):

- **This device owns it** whenever the band is written or Auto is selected from HomeKit, the web UI or a Serin Link dial. The engine steers as described above. This is the default, and the behaviour of every release before 0.2.9.
- **The unit owns it** when the heat pump reports an Auto setpoint, or an entry into Auto, that this device did not command — a change made at the wall or with the remote. The engine goes silent and the unit's setpoint `S` is imported as the band `[S−1, S+1]` so the Home app, the web UI and the dial all show a range consistent with the wall. Control returns to this device on the next band edit or Auto selection made here. The web UI's Auto card says "Set at the heat pump" while the unit owns it.

The controller tells the two apart with its own command state: a reported change is ours while the matching field's latest SET has been transmitted and is inside the 10 s grace window (so a unit clamping our value never counts, and a fan or vane command sent meanwhile doesn't change that), and the first SETTINGS response after every connect is a baseline, never a change.

**Setpoints the unit won't hold.** A unit or its wall controller may refuse part of the range — for example an Auto floor of 19 °C (67 °F). When the grace window ends with the unit reporting a different setpoint than the one sent, the engine treats that as the unit's answer and does not send the same value again while the unit keeps reporting that answer in the same mode (`CN105Controller::refusedSetpoint`). If the unit did not move at all, the SET may have been lost, so the value gets one retry first. The refusal is forgotten when the engine's target changes, the unit reports something else, the mode changes or the link reconnects. Without this, an out-of-range band re-sent its target every grace window, and a wall-controller change made inside that window was credited to this device and overwritten.

**Changeover is performed by the heat pump, not the controller.** In AUTO the firmware sets the single target temperature but does not force the heat/cool direction — the unit decides, switching Cool↔Heat once the room is about 4°F (2°C) past the set temperature. So after the room crosses a threshold the switch can lag, and a small room-vs-threshold gap may not trigger one at all (e.g. room 74°F with the cooling threshold at 70°F can stay latched on heat). This is expected manufacturer behavior; the web UI surfaces a short note in the AUTO card so users aren't surprised by the delay. On a multi-zone (MXZ) system Mitsubishi advises against Auto altogether: an idle head left in Auto holds the outdoor unit's mode and can block the other zones.

## Vane Control

Vane and wide vane positions (including swing) are set through the [web UI](../README.md#web-ui). HomeKit has no vane characteristic, so the only way to change airflow direction from the Home app, a scene or Siri is the optional **Vane Switch** below.

### Vane Switch

An opt-in Switch service that applies a saved vane preset. In **Settings › HomeKit** you save two presets, each a vane position plus a wide vane position: **On** and **Off**. Turning the switch on in the Home app sends the On preset; turning it off sends the Off preset. The switch changes vanes only — never power, mode, fan speed or setpoint — so a scene that heats a second room sets the thermostat and the switch together.

- **Enabling or disabling the switch needs a restart.** HomeKit services are fixed when the firmware starts; the web UI shows a reboot prompt until then. Editing a preset applies immediately.
- **The switch mirrors the unit.** It reads On only while the current vane and wide vane match the On preset. A change made with the remote, the wall controller or the web UI flips it to Off.
- **The two presets must differ.** The switch cannot be enabled while On and Off are the same position, and while it is on an edit that would make them equal is refused.
- **Vane Type gates it.** On a unit declared as Vane only, presets hold a vane position alone and the wide vane choice is hidden. With Vane Type set to None, the controls are hidden and the switch is not created. Saved presets survive a Vane Type change.
- The default name in the Home app is "Vane Preset"; rename it like any other accessory.

## Temperature

Setpoints range from 16°C to 30.5°C. The upper bound is 30.5 rather than the protocol's 31 so that the °C and °F ladders stay symmetric — 31°C has no distinct °F step of its own. The web UI offers a °C/°F display toggle, but the protocol always operates in Celsius. Half-degree precision is supported when the unit's enhanced temperature encoding is detected.

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
