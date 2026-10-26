#include "auto_band.h"
#include "setpoint_control.h"

bool AutoBand::deviceOwns() {
    return SetpointControl::autoDeviceOwns();
}

void AutoBand::loop(CN105Controller&) {
    SetpointControl::updateAutoOwnership();
}
