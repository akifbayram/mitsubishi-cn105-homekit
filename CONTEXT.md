# Mitsubishi CN105 HomeKit

Firmware that bridges a Mitsubishi heat pump's CN105 service port to HomeKit and a local web UI. This glossary fixes the words the code, docs, and web UI use for the concepts that are specific to this project.

## Language

### Airflow

**Vane**:
The vertical louver that steers air up or down. Positions are Auto, 1 (most horizontal) through 5 (most vertical), and Swing.
_Avoid_: Louver, flap, vertical vane

**Wide Vane**:
The horizontal louver that steers air left or right. Positions are Left-Left, Left, Center, Right, Right-Right, Split, and Swing.
_Avoid_: Horizontal vane, H-vane

**Vane Type**:
The owner's declaration of which vanes the indoor unit has: none, Vane only, or Vane and Wide Vane.
_Avoid_: Vane config

**Vane Preset**:
One saved pair of a Vane position and a Wide Vane position. On a Vane-only unit it holds a Vane position alone.
_Avoid_: Saved vane position, airflow preset, scene

**Vane Switch**:
The optional HomeKit Switch service that applies one Vane Preset when turned On and another when turned Off. It reads On only while the unit's current Vane and Wide Vane match the On preset.
_Avoid_: Vane scene, preset switch

### HomeKit

**Hidden Service**:
One of Fan, Fan Auto, Fan Mode or Dry Mode that the owner removed from the Heat Pump accessory under Settings › HomeKit › Services. It is not built at the next boot, and every service that stays keeps its HomeKit instance IDs.
_Avoid_: Disabled service, removed tile
