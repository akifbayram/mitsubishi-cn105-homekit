#pragma once

#include "cn105_protocol.h"

// ════════════════════════════════════════════════════════════════════════════
// Auto band ownership.
//
// HomeKit's Auto is a heating/cooling range; the heat pump's Auto is one
// setpoint. The dual-setpoint engine (homekit_sync_thermostat) steers that
// setpoint from the range — and a wall controller or IR remote writes the
// same register. Two writers, one register: the last one to touch it wins.
//
//   device owns  — the engine steers. Set by every band write or Auto
//                  selection made through this device (HomeKit, web, dial).
//   unit owns    — the engine is silent. Set when the controller reports an
//                  Auto setpoint or Auto entry we did not command
//                  (CN105Controller::externalAutoChanges); the unit's setpoint
//                  is imported as the band [S-1, S+1] so every surface shows
//                  a range consistent with what the wall shows.
//
// Persisted (DeviceSettings::autoOwnerUnit) so a power cycle changes nothing.
//
// Single setpoint mode (settings.autoSingleBoot()) drops the range: every
// surface edits the unit's one Auto setpoint, as in Heat and Cool, so the
// engine never steers and ownership never changes. Saving Range again hands
// Auto to the unit (SetpointControl::saveAutoSingle).
// ════════════════════════════════════════════════════════════════════════════

// Minimum heating→cooling threshold gap. One rule for the web UI's clamp,
// the dial's CAPS and the import below.
constexpr float AUTO_BAND_MIN_GAP_C = 2.0f;

namespace AutoBand {

/// True while the band engine may steer: Range mode, and this device last
/// wrote the Auto operating point.
bool deviceOwns();

/// Main-loop poll. Hands over on each external Auto change the controller
/// has counted since the last call. The shared coordinator defers handover
/// while a custom-range correction is awaiting confirmation.
void loop(CN105Controller &cn105);

}  // namespace AutoBand
