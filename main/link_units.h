/*
 * link_units.h — unit conversions for the dial's DIAL_SENSOR reading.
 *
 * Proto v3 put temperature and humidity on the air in centi (0.01 C, 0.01 %).
 * Everything downstream of here still speaks deci: the room-feed change grid,
 * the per-source calibration offsets, and the CN105 wire itself, which
 * quantizes to 0.5 C. So the conversion happens once, here, and the finer
 * value survives only where it is actually worth something — the web tile and
 * the telemetry that reads it. Humidity needs no conversion: it is carried as
 * centi-% and divided once, at the accessor.
 *
 * Pure and dependency-free, same discipline as room_feed.h, so test/
 * sl2_dial_sensor can prove it without a CN105, a radio, or FreeRTOS.
 */
#pragma once
#include <stdint.h>
#include "sl2_proto.h"

/* Wire centi-C -> deci-C. Rounds half AWAY FROM ZERO: C truncates toward
 * zero, which would round a negative reading warm and put the two signs on
 * different grids. SL2_CC_NA is a sentinel, not a temperature — carried
 * across as the deci sentinel rather than run through the arithmetic, where
 * 0x7FFF + 5 would wrap into a plausible-looking 327.7 C. */
static inline int16_t link_cc_to_dc(int16_t cc) {
    if (cc == SL2_CC_NA) return SL2_DC_NA;
    return (int16_t)((cc >= 0 ? cc + 5 : cc - 5) / 10);
}
