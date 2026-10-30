#pragma once

#include "cn105_protocol.h"
#include "settings.h"

namespace SetpointControl {
struct Command {
    CN105Command unit;
    std::optional<float> heatingThreshold, coolingThreshold;
};
enum class Condition : uint8_t { Satisfied, Pending, Refused, Incompatible };
struct Status { Condition condition; float actualC, desiredC; bool targetReadable; };
// One policy observation for serializers/export. UART readback can advance
// independently; saved preferences and resolved policy share the coordinator lock.
struct SettingsView {
    SetpointSettings saved;
    SetpointRange::Snapshot ranges;
    SetpointRange::Band effectiveBand; // Operational thresholds; saved.band retains intent.
    SetpointRange::Detection reference;
    Status status;
};
SettingsView settingsView();
void begin(CN105Controller&);
SetpointRange::Snapshot snapshot();
Status status();
SetpointRange::Result submit(const Command&);
SetpointRange::Result applySettings(const SetpointSettings&);
void loop();
// Auto ownership updates share the command/configuration mutex and checked store.
bool autoDeviceOwns();
void updateAutoOwnership();
// Recompute and conditionally stage internal Auto steering under the coordinator lock.
void steerAuto();
// Save the Auto setpoint mode for the next boot. Saving Range while this boot
// runs Single first hands Auto to the unit, so the restart cannot steer from a
// stale band; if that handover cannot be stored the device stays Single.
SetpointRange::Result saveAutoSingle(bool single);
}
