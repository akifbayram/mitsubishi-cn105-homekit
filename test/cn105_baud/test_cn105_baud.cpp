// Host tests for the CN105 baud-rate rules (cn105_baud.h): which rates the
// firmware accepts from the web UI or an imported settings file.
#include "cn105_baud.h"
#include <cassert>
#include <cstdio>

int main() {
    // The two rates Mitsubishi indoor units speak on CN105
    assert(cn105_baud_valid(2400));
    assert(cn105_baud_valid(9600));

    // Anything else is rejected — other standard rates, near misses, zero
    // (a missing or unparsable number) and negatives
    const long rejected[] = {0, 1200, 4800, 19200, 38400, 115200,
                             2399, 2401, 9599, 9601, -2400, -9600};
    for (long baud : rejected)
        assert(!cn105_baud_valid(baud));

    // A fresh device, a factory reset, or firmware from before the setting
    // existed: no rate stored (the load's 0 seed survives), so the unit runs
    // at 2400
    assert(cn105_baud_load(0) == 2400);

    // A stored choice survives a reboot or an OTA update
    assert(cn105_baud_load(2400) == 2400);
    assert(cn105_baud_load(9600) == 9600);

    // A stored value the firmware never accepts (corrupt NVS, a newer
    // firmware's rate after a downgrade) is not put on the wire
    assert(cn105_baud_load(4800) == 2400);
    assert(cn105_baud_load(115200) == 2400);

    printf("cn105_baud: all tests passed\n");
    return 0;
}
