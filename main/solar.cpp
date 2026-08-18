#include "solar.h"

#include <cmath>

static constexpr double DEG = M_PI / 180.0;
static constexpr double CIVIL_TWILIGHT_DEG = -6.0;

// Core of the Astronomical Almanac low-precision form: elevation in degrees,
// and (optionally) the hour angle in radians, folded to [-pi,+pi) — negative
// before local solar noon, positive after. The sign is what lets the offset
// variant tell a dusk crossing from a dawn crossing without ever computing
// event times.
static double solar_elevation(uint32_t epoch, float lat_deg, float lon_deg,
                              double *H_out) {
    // Days since J2000.0 (2000-01-01 12:00 UTC = unix 946728000).
    double d = ((double)epoch - 946728000.0) / 86400.0;
    // Mean anomaly and mean longitude of the sun (degrees).
    double g = fmod(357.529 + 0.98560028 * d, 360.0) * DEG;
    double q = fmod(280.459 + 0.98564736 * d, 360.0);
    // Ecliptic longitude, declination (obliquity drift negligible here).
    double L = (q + 1.915 * sin(g) + 0.020 * sin(2.0 * g)) * DEG;
    double e = 23.439 * DEG;
    double decl = asin(sin(e) * sin(L));
    // Equation of time (hours), normalized to +/-12.
    double ra_h = fmod(atan2(cos(e) * sin(L), cos(L)) / DEG / 15.0 + 24.0, 24.0);
    double eqt = q / 15.0 - ra_h;
    eqt = fmod(eqt + 36.0, 24.0) - 12.0;
    // True solar time -> hour angle -> elevation.
    double ut_h = fmod((double)epoch, 86400.0) / 3600.0;
    double tst_h = ut_h + (double)lon_deg / 15.0 + eqt;
    // Fold to [-12,+12) hours: true solar time crosses the UTC day boundary
    // at any non-zero longitude, and cos() hides that from the elevation but
    // not from the sign the offset variant reads. fmod keeps the sign of its
    // argument, so bias into positives before the second fold.
    double h_ang = fmod(fmod(tst_h - 12.0, 24.0) + 36.0, 24.0) - 12.0;
    double H = h_ang * 15.0 * DEG;
    if (H_out) *H_out = H;
    double lat = (double)lat_deg * DEG;
    double sin_el = sin(lat) * sin(decl) + cos(lat) * cos(decl) * cos(H);
    return asin(sin_el) / DEG;
}

solar_gate_t solar_gate_at(uint32_t epoch, float lat_deg, float lon_deg) {
    // The zero-offset special case of the offset variant — one home for the
    // guards and the twilight threshold, so they can never drift apart.
    return solar_gate_off(epoch, lat_deg, lon_deg, 0, 0);
}

solar_gate_t solar_gate_off(uint32_t epoch, float lat_deg, float lon_deg,
                            int dusk_off_min, int dawn_off_min) {
    if (epoch == 0 || std::isnan(lat_deg) || std::isnan(lon_deg))
        return SOLAR_NO_FIX;
    // Which edge are we near? The hour angle's sign says: past solar noon the
    // next crossing is dusk, before it the last crossing was dawn. Shifting
    // the EVALUATION TIME back by the offset moves that edge later by exactly
    // the offset (and earlier for negative values) — no event-time solver
    // needed, and the polar constant-answer behavior is preserved (deep
    // night/day is deep night/day at t and t-off alike).
    double H;
    (void)solar_elevation(epoch, lat_deg, lon_deg, &H);
    int off_min = (H > 0.0) ? dusk_off_min : dawn_off_min;
    double shifted = (double)epoch - (double)off_min * 60.0;
    if (shifted < 1.0) shifted = 1.0;   // guard stays "clock valid"
    double el = solar_elevation((uint32_t)shifted, lat_deg, lon_deg, nullptr);
    return (el < CIVIL_TWILIGHT_DEG) ? SOLAR_NIGHT : SOLAR_DAY;
}

#ifdef SOLAR_SELFTEST
#include <cassert>
#include <cstdio>
int main() {
    // 1782000000 = 2026-06-21 00:00:00 UTC (solstice).
    // London (51.48N, 0E) local midnight: min elevation ~ -15 deg -> NIGHT.
    assert(solar_gate_at(1782000000, 51.48f, 0.0f) == SOLAR_NIGHT);
    // London solstice noon (12:00 UTC): ~62 deg -> DAY.
    assert(solar_gate_at(1782043200, 51.48f, 0.0f) == SOLAR_DAY);
    // Tromso (69.65N) solstice local midnight (23:00 UTC ~ solar midnight
    // at 18.96E): midnight sun, elevation ~ +3 deg -> DAY. Pins polar
    // behavior: a constant answer, never an oscillation.
    assert(solar_gate_at(1782082800, 69.65f, 18.96f) == SOLAR_DAY);
    // Quito (0.18S, 78.47W) 18:30 UTC = 13:30 local -> DAY.
    assert(solar_gate_at(1782066600, -0.18f, -78.47f) == SOLAR_DAY);
    // Quito 06:00 UTC = 01:00 local -> NIGHT (equatorial twilight is short).
    assert(solar_gate_at(1782021600, -0.18f, -78.47f) == SOLAR_NIGHT);
    // Guards: no clock / no location -> NO_FIX.
    assert(solar_gate_at(0, 51.48f, 0.0f) == SOLAR_NO_FIX);
    assert(solar_gate_at(1782000000, NAN, 0.0f) == SOLAR_NO_FIX);

    // ---- offsets ----
    // Zero offsets are the identity.
    assert(solar_gate_off(1782000000, 51.48f, 0.0f, 0, 0) == SOLAR_NIGHT);
    assert(solar_gate_off(1782043200, 51.48f, 0.0f, 0, 0) == SOLAR_DAY);
    // Dusk edge (London solstice, civil dusk ~21:07 UTC): 21:30 UTC
    // (1782077400, el ~ -8 deg) is raw NIGHT; a +60 min dusk offset
    // re-evaluates 20:30 (el ~ -2 deg) -> still DAY. Dawn offset must not
    // touch the evening side.
    assert(solar_gate_at(1782077400, 51.48f, 0.0f) == SOLAR_NIGHT);
    assert(solar_gate_off(1782077400, 51.48f, 0.0f, 60, 0) == SOLAR_DAY);
    assert(solar_gate_off(1782077400, 51.48f, 0.0f, 0, 60) == SOLAR_NIGHT);
    // Dawn edge (civil dawn ~02:55 UTC): 03:30 UTC (1782012600, el ~ -2 deg)
    // is raw DAY; a +60 min dawn offset re-evaluates 02:30 (el ~ -8 deg) ->
    // still NIGHT. Dusk offset must not touch the morning side.
    assert(solar_gate_at(1782012600, 51.48f, 0.0f) == SOLAR_DAY);
    assert(solar_gate_off(1782012600, 51.48f, 0.0f, 0, 60) == SOLAR_NIGHT);
    assert(solar_gate_off(1782012600, 51.48f, 0.0f, 60, 0) == SOLAR_DAY);
    // Shift equivalence by construction: off = evaluate at t - off.
    assert(solar_gate_off(1782077400, 51.48f, 0.0f, 120, 0)
           == solar_gate_at(1782077400 - 7200, 51.48f, 0.0f));
    assert(solar_gate_off(1782012600, 51.48f, 0.0f, 0, 120)
           == solar_gate_at(1782012600 - 7200, 51.48f, 0.0f));
    // Non-zero longitude: true solar time crosses the UTC day boundary there,
    // so the hour angle has to be folded before its sign picks an edge.
    // Los Angeles (34.05N, 118.24W) solstice dusk ~03:37 UTC on the NEXT UTC
    // day while TST is still afternoon: 04:00 UTC is raw NIGHT, a +60 min
    // dusk offset re-evaluates 03:00 -> DAY, and a dawn offset must not move
    // this edge at all.
    assert(solar_gate_at(1782014400, 34.05f, -118.24f) == SOLAR_NIGHT);
    assert(solar_gate_off(1782014400, 34.05f, -118.24f, 60, 0) == SOLAR_DAY);
    assert(solar_gate_off(1782014400, 34.05f, -118.24f, 0, 60) == SOLAR_NIGHT);
    // LA dawn ~12:13 UTC: 12:30 is raw DAY, +60 min dawn offset -> NIGHT,
    // dusk offset inert.
    assert(solar_gate_at(1782045000, 34.05f, -118.24f) == SOLAR_DAY);
    assert(solar_gate_off(1782045000, 34.05f, -118.24f, 0, 60) == SOLAR_NIGHT);
    assert(solar_gate_off(1782045000, 34.05f, -118.24f, 60, 0) == SOLAR_DAY);
    // Tokyo (35.68N, 139.65E) is the eastern mirror — TST runs past 24 h, so
    // its dawn ~18:56 UTC lands on the previous UTC day. Dusk ~10:31 UTC:
    // 11:00 is raw NIGHT, +60 min dusk -> DAY.
    assert(solar_gate_at(1782039600, 35.68f, 139.65f) == SOLAR_NIGHT);
    assert(solar_gate_off(1782039600, 35.68f, 139.65f, 60, 0) == SOLAR_DAY);
    assert(solar_gate_off(1782039600, 35.68f, 139.65f, 0, 60) == SOLAR_NIGHT);
    // Tokyo dawn edge: 19:20 UTC is raw DAY, +60 min dawn -> NIGHT.
    assert(solar_gate_at(1782069600, 35.68f, 139.65f) == SOLAR_DAY);
    assert(solar_gate_off(1782069600, 35.68f, 139.65f, 0, 60) == SOLAR_NIGHT);
    assert(solar_gate_off(1782069600, 35.68f, 139.65f, 60, 0) == SOLAR_DAY);
    // Shift equivalence away from the prime meridian too.
    assert(solar_gate_off(1782014400, 34.05f, -118.24f, 120, 0)
           == solar_gate_at(1782014400 - 7200, 34.05f, -118.24f));
    assert(solar_gate_off(1782069600, 35.68f, 139.65f, 0, 120)
           == solar_gate_at(1782069600 - 7200, 35.68f, 139.65f));
    // Guards hold for the offset variant too.
    assert(solar_gate_off(0, 51.48f, 0.0f, 30, 30) == SOLAR_NO_FIX);
    assert(solar_gate_off(1782000000, NAN, 0.0f, 30, 30) == SOLAR_NO_FIX);
    printf("solar selftest ok\n");
    return 0;
}
#endif
