#pragma once
// Sun-down gate for the Serin Link night feature (SL2_SF2_NIGHT_*).
// Pure: epoch in, verdict out — no clock or settings dependency, so the
// math is host-testable (see SOLAR_SELFTEST in solar.cpp).

#include <cstdint>

enum solar_gate_t {
    SOLAR_NO_FIX = 0,   // clock unsynced (epoch 0) or location unset (NaN)
    SOLAR_DAY,
    SOLAR_NIGHT,        // solar elevation below civil twilight (-6 deg)
};

// Low-precision solar elevation (Astronomical Almanac approximation,
// good to ~0.5 deg — plenty against a -6 deg threshold). UTC epoch only;
// timezone deliberately does not exist in this feature.
solar_gate_t solar_gate_at(uint32_t epoch, float lat_deg, float lon_deg);

// Same gate with per-edge minute offsets: positive dusk_off_min starts night
// that many minutes AFTER civil dusk; positive dawn_off_min ends it that many
// minutes after civil dawn (negative values move each edge earlier). The
// hour angle's sign picks which offset applies, and the gate is evaluated at
// the shifted time — exact minute shifts with no sunrise/sunset solver.
solar_gate_t solar_gate_off(uint32_t epoch, float lat_deg, float lon_deg,
                            int dusk_off_min, int dawn_off_min);
