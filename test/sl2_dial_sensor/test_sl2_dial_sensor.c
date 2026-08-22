/* Host tests for the DIAL_SENSOR wire contract — specifically its UNITS.
 *
 * Proto v3 rescaled temp/hum from deci to centi without changing the packet
 * size, so a stale vendored sl2_proto.h decodes a v3 frame cleanly into the
 * wrong units: 23.53 C arrives as 235.3 C and the web UI renders 455.5 F.
 * Every sizeof guard still passes while that happens, and the reading looks
 * like a real number rather than an error — so the skew has to be caught
 * here, at build time, in the copy that does the decoding. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "sl2_proto.h"
#include "link_units.h"

/* Exactly what a v3 dial puts on the air for 23.53 C / 31.03 %RH. Hand-rolled
 * rather than built from the struct: a golden frame that agrees with the
 * struct only because both were edited together proves nothing. */
static const unsigned char GOLDEN_V3[9] = {
    SL2_PKT_DIAL_SENSOR,
    3,                    /* version */
    SL2_DSF_HAS_SENSOR,
    0x31, 0x09,           /* temp_cc = 2353 centi-C = 23.53 C   (little-endian) */
    0x1F, 0x0C,           /* hum_cc  = 3103 centi-% = 31.03 %RH (little-endian) */
    SL2_ROOMSRC_NOEDIT,
    0x00,                 /* reserved */
};

static void test_wire_layout(void) {
    assert(sizeof(struct sl2_dial_sensor_pkt) == 9);
    assert(offsetof(struct sl2_dial_sensor_pkt, temp_cc)  == 3);
    assert(offsetof(struct sl2_dial_sensor_pkt, hum_cc)   == 5);
    assert(offsetof(struct sl2_dial_sensor_pkt, want_src) == 7);
    /* MIN_LEN must cover hum_cc, which is 2 bytes wide as of v3. A MIN_LEN of
     * 6 accepts a frame whose humidity is half-present. */
    assert(SL2_DIAL_SENSOR_MIN_LEN == 7);
}

static void test_version_floor_is_pinned(void) {
    /* The floor is a historical fact, not an alias for the current version:
     * if it tracked SL2_PROTO_VERSION it would stop rejecting v2 at the next
     * bump. See its definition in sl2_proto.h. */
    assert(SL2_DIAL_SENSOR_MIN_VER == 3);
    assert(SL2_PROTO_VERSION >= SL2_DIAL_SENSOR_MIN_VER);
    /* A v2 dial must be dropped, not decoded — that is the 455.5 F bug. */
    assert(2 < SL2_DIAL_SENSOR_MIN_VER);
}

static void test_sentinels_are_distinct_widths(void) {
    assert(SL2_CC_NA     == (int16_t)0x7FFF);
    assert(SL2_HUM_CC_NA == (uint16_t)0xFFFF);
    /* The 8-bit humidity sentinel belongs to sl2_state_pkt only. Mixing them
     * compares false forever and reads as "never reports". */
    assert((uint16_t)SL2_HUM_NA != SL2_HUM_CC_NA);
}

static void test_golden_frame_decodes_to_room_temperature(void) {
    struct sl2_dial_sensor_pkt p;
    sl2_decode_pkt(&p, sizeof p, GOLDEN_V3, (int)sizeof GOLDEN_V3);

    assert(p.type    == SL2_PKT_DIAL_SENSOR);
    assert(p.version == 3);
    assert(p.flags   &  SL2_DSF_HAS_SENSOR);
    assert(p.temp_cc == 2353);
    assert(p.hum_cc  == 3103);

    /* The whole point: a room, not an oven. Reading these bytes as deci gives
     * 235.3 C, which the web UI renders as 455.5 F. */
    const float c = p.temp_cc / 100.0f;
    assert(c > 23.0f && c < 24.0f);
    const float f = c * 9.0f / 5.0f + 32.0f;
    assert(f > 74.0f && f < 75.0f);

    const float rh = p.hum_cc / 100.0f;
    assert(rh > 31.0f && rh < 31.1f);
}

static void test_short_frame_leaves_want_src_absent(void) {
    /* A sender that stops after hum_cc is reading-only. The tolerant decode
     * zero-fills want_src, and 0 is Internal — so the caller must gate on
     * LENGTH, never on the decoded value. */
    struct sl2_dial_sensor_pkt p;
    sl2_decode_pkt(&p, sizeof p, GOLDEN_V3, SL2_DIAL_SENSOR_MIN_LEN);
    assert(p.temp_cc == 2353);
    assert(p.hum_cc  == 3103);
    assert(p.want_src == 0);   /* zero-filled, NOT an edit to Internal */
    assert((size_t)SL2_DIAL_SENSOR_MIN_LEN <=
           offsetof(struct sl2_dial_sensor_pkt, want_src));
}

static void test_centi_to_deci_rounds_to_nearest(void) {
    assert(link_cc_to_dc(2353) == 235);   /* 23.53 -> 23.5 */
    assert(link_cc_to_dc(2355) == 236);   /* half rounds away from zero */
    assert(link_cc_to_dc(2349) == 235);
    assert(link_cc_to_dc(0)    == 0);
    /* Negatives must not drift warm: truncation would round -23.55 to -23.5. */
    assert(link_cc_to_dc(-2355) == -236);
    assert(link_cc_to_dc(-2353) == -235);
    /* The sentinel is a value, not a temperature: 0x7FFF + rounding wraps. */
    assert(link_cc_to_dc(SL2_CC_NA) == SL2_DC_NA);
}

int main(void) {
    test_wire_layout();
    test_version_floor_is_pinned();
    test_sentinels_are_distinct_widths();
    test_golden_frame_decodes_to_room_temperature();
    test_short_frame_leaves_want_src_absent();
    test_centi_to_deci_rounds_to_nearest();
    printf("sl2_dial_sensor: all tests passed\n");
    return 0;
}
