// Host-side tests for the BLE advertisement decoders (Govee, PVVX, BTHome,
// SwitchBot). Compiled and run by run.sh — no hardware or ESP-IDF needed.
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include "ble_decoders.h"

static int g_checks = 0;
#define CHECK(c) do { assert(c); g_checks++; } while (0)

static bool feq(float a, float b) { return std::fabs(a - b) < 0.05f; }
static bool isSB(const char* t) { return t && !strcmp(t, "SwitchBot"); }

// Decode a single-buffer advertisement (no split, so MAC/cache not needed).
static bool isType(const uint8_t* adv, size_t len, const char* want, SensorReading& out) {
    const char* t = decodeAdvertisement(adv, len, nullptr, out);
    return t && !strcmp(t, want);
}

// ── SwitchBot temp+hum block math (masking, sign, boundaries) ────────────────
static void test_switchbot_temphum(void) {
    SensorReading r;
    // 29.8 C / 42 % from the real Meter Pro capture (08 9d 2a)
    uint8_t ok[] = {0x08, 0x9d, 0x2a};
    CHECK(decodeSwitchBotTempHum(ok, r) && feq(r.temp, 29.8f) && feq(r.hum, 42));
    // Alert-flag bits in the high nibble of byte 0 are masked, not read
    SensorReading r2; uint8_t alert[] = {0xf8, 0x9d, 0x2a};
    CHECK(decodeSwitchBotTempHum(alert, r2) && feq(r2.temp, 29.8f));
    // Negative: bit7 of byte 1 clear -> -40.0 at the boundary
    SensorReading r3; uint8_t neg[] = {0x00, 0x28, 0x2a};
    CHECK(decodeSwitchBotTempHum(neg, r3) && feq(r3.temp, -40.0f));
    // Positive boundary 80.0 accepted, 80.1 rejected
    SensorReading r4; uint8_t hi[]  = {0x00, 0xd0, 0x2a};   // 80.0
    SensorReading r5; uint8_t hi2[] = {0x01, 0xd0, 0x2a};   // 80.1
    CHECK(decodeSwitchBotTempHum(hi, r4) && feq(r4.temp, 80.0f));
    CHECK(!decodeSwitchBotTempHum(hi2, r5));
    // frac digit > 9 and humidity > 100 rejected
    SensorReading r6; uint8_t badf[] = {0x0a, 0x94, 0x2a};
    SensorReading r7; uint8_t badh[] = {0x00, 0x94, 0x65};
    CHECK(!decodeSwitchBotTempHum(badf, r6));
    CHECK(!decodeSwitchBotTempHum(badh, r7));
}

// ── SwitchBot Meter Pro: split across ADV_IND (0xFD3D) + SCAN_RSP (0x0969) ───
static void test_switchbot_meterpro_split(void) {
    s_sbTypeCacheCount = 0;
    const char* mac = "b0:e9:fe:80:3d:e1";
    // Real captures
    uint8_t adv[] = {0x02,0x01,0x06, 0x06,0x16,0x3d,0xfd,0x34,0x00,0x64};
    uint8_t rsp[] = {0x10,0xff,0x69,0x09,0xb0,0xe9,0xfe,0x80,0x3d,0xe1,
                     0x3c,0x64,0x08,0x9d,0x2a,0x03,0x0e};
    // ADV_IND: type + battery, no temperature yet
    SensorReading a; const char* ta = decodeAdvertisement(adv, sizeof adv, mac, a);
    CHECK(isSB(ta) && a.batt == 100 && std::isnan(a.temp));
    // SCAN_RSP: 0x0969 has no 0xFD3D, but the cached meter type lets temp decode
    SensorReading b; const char* tb = decodeAdvertisement(rsp, sizeof rsp, mac, b);
    CHECK(isSB(tb) && feq(b.temp, 29.8f) && feq(b.hum, 42));
}

// ── Gate holds across the cache: a non-meter SwitchBot must never decode ─────
static void test_switchbot_nonmeter_gate(void) {
    s_sbTypeCacheCount = 0;
    const char* mac = "aa:bb:cc:dd:ee:ff";
    // Curtain (devType 'c') advertisement caches type 'c'
    uint8_t adv[] = {0x02,0x01,0x06, 0x06,0x16,0x3d,0xfd,0x63,0x00,0x64};
    // Same 0x0969 payload as a meter — must stay gated out
    uint8_t rsp[] = {0x10,0xff,0x69,0x09,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
                     0x3c,0x64,0x08,0x9d,0x2a,0x03,0x0e};
    SensorReading a; const char* ta = decodeAdvertisement(adv, sizeof adv, mac, a);
    CHECK(ta == nullptr && std::isnan(a.temp));
    SensorReading b; const char* tb = decodeAdvertisement(rsp, sizeof rsp, mac, b);
    CHECK(tb == nullptr && std::isnan(b.temp));
    // A cold SCAN_RSP with no prior 0xFD3D anywhere also stays gated
    s_sbTypeCacheCount = 0;
    SensorReading c; const char* tc = decodeAdvertisement(rsp, sizeof rsp, mac, c);
    CHECK(tc == nullptr && std::isnan(c.temp));
}

// ── SwitchBot Meter ('T'): everything in the 0xFD3D service data ─────────────
static void test_switchbot_meter_single(void) {
    s_sbTypeCacheCount = 0;
    const char* mac = "11:22:33:44:55:66";
    uint8_t adv[] = {0x09,0x16,0x3d,0xfd, 0x54,0x00,0x64,0x08,0x9d,0x2a};
    SensorReading r; const char* t = decodeAdvertisement(adv, sizeof adv, mac, r);
    CHECK(isSB(t) && feq(r.temp, 29.8f) && feq(r.hum, 42) && r.batt == 100);
}

// ── Govee V3 (0xEC88, combined @3), V2 (LE), V1 (0x0001, combined @4) ────────
// 0xEC88 frame lengths (incl the 2-byte company id) tell the families apart,
// per the govee-ble reference: H5072/H5075 = 8 (older firmware 7), H5074 = 9,
// H5051 = 11.
static void test_govee(void) {
    // H5072/H5075: 8 bytes with a trailer byte; combined 250500 -> 25.0 C / 50.0 %
    SensorReading v3; uint8_t g3[] = {0x09,0xff,0x88,0xec,0x00,0x03,0xd2,0x84,0x64,0x00};
    CHECK(isType(g3, sizeof g3, "Govee", v3) && feq(v3.temp,25.0f) && feq(v3.hum,50.0f) && v3.batt==100);
    // Older H5072/H5075 firmware: same layout without the trailer (7 bytes)
    SensorReading v3s; uint8_t g3s[] = {0x08,0xff,0x88,0xec,0x00,0x03,0xd2,0x84,0x64};
    CHECK(isType(g3s, sizeof g3s, "Govee", v3s) && feq(v3s.temp,25.0f) && feq(v3s.hum,50.0f) && v3s.batt==100);
    // Sign bit (0x800000) in the combined value: -10.1 C / 40.0 %
    SensorReading v3n; uint8_t g3n[] = {0x09,0xff,0x88,0xec,0x00,0x81,0x8c,0x18,0x64,0x00};
    CHECK(isType(g3n, sizeof g3n, "Govee", v3n) && feq(v3n.temp,-10.1f) && feq(v3n.hum,40.0f));
    // H5074: 9 bytes, little-endian: 22.55 C / 45.10 %
    SensorReading v2; uint8_t g2[] = {0x0a,0xff,0x88,0xec,0x00,0xcf,0x08,0x9e,0x11,0x5a,0x00};
    CHECK(isType(g2, sizeof g2, "Govee", v2) && feq(v2.temp,22.55f) && feq(v2.hum,45.10f) && v2.batt==90);
    // H5051: 11 bytes, same layout with a longer tail
    SensorReading v2l; uint8_t g2l[] = {0x0c,0xff,0x88,0xec,0x00,0xcf,0x08,0x9e,0x11,0x5a,0x00,0x00,0x00};
    CHECK(isType(g2l, sizeof g2l, "Govee", v2l) && feq(v2l.temp,22.55f) && feq(v2l.hum,45.10f) && v2l.batt==90);
    // An 8-byte frame is V3 by length; V2-shaped bytes must not decode as anything
    SensorReading no; uint8_t g8[] = {0x09,0xff,0x88,0xec,0x00,0xcf,0x08,0x9e,0x11,0x5a};
    CHECK(decodeAdvertisement(g8, sizeof g8, nullptr, no) == nullptr);
    // V1 (company 0x0001), combined @4
    SensorReading v1; uint8_t g1[] = {0x09,0xff,0x01,0x00,0x00,0x00,0x03,0xd2,0x84,0x64};
    CHECK(isType(g1, sizeof g1, "Govee", v1) && feq(v1.temp,25.0f) && feq(v1.hum,50.0f) && v1.batt==100);
}

// ── PVVX custom format (0x181A, 15 bytes, little-endian) ────────────────────
static void test_pvvx(void) {
    // Full frame: MAC, temp 21.00, hum 55.00, 2900 mV, batt 95, counter, flags
    SensorReading p; uint8_t pv[] = {0x12,0x16,0x1a,0x18, 0xaa,0xbb,0xcc,0xdd,0xee,0xff,
                                     0x34,0x08, 0x7c,0x15, 0x54,0x0b, 0x5f, 0x01, 0x04};
    CHECK(isType(pv, sizeof pv, "PVVX", p) && feq(p.temp,21.0f) && feq(p.hum,55.0f) && p.batt==95);
}

// ── ATC1441 format (0x181A, 13 bytes, big-endian) ───────────────────────────
static void test_atc1441(void) {
    // 24.0 C / 45 % / batt 93 / 2900 mV / counter
    SensorReading a; uint8_t at[] = {0x10,0x16,0x1a,0x18, 0xa4,0xc1,0x38,0xaa,0xbb,0xcc,
                                     0x00,0xf0, 0x2d, 0x5d, 0x0b,0x54, 0x21};
    CHECK(isType(at, sizeof at, "ATC1441", a) && feq(a.temp,24.0f) && feq(a.hum,45.0f) && a.batt==93);
    // Temperature is signed: -8.3 C
    SensorReading n; uint8_t ng[] = {0x10,0x16,0x1a,0x18, 0xa4,0xc1,0x38,0xaa,0xbb,0xcc,
                                     0xff,0xad, 0x2d, 0x5d, 0x0b,0x54, 0x21};
    CHECK(isType(ng, sizeof ng, "ATC1441", n) && feq(n.temp,-8.3f));
    // Regression: 13-byte ATC frames used to reach the PVVX decoder, which could
    // pass validation with garbage (25.6 C read byte-swapped as 0.01 C, hum and
    // battery merged into 90.05 %). Must decode as ATC1441 with the true values.
    SensorReading r; uint8_t rg[] = {0x10,0x16,0x1a,0x18, 0xa4,0xc1,0x38,0xaa,0xbb,0xcc,
                                     0x01,0x00, 0x2d, 0x23, 0x0b,0x54, 0x21};
    CHECK(isType(rg, sizeof rg, "ATC1441", r) && feq(r.temp,25.6f) && feq(r.hum,45.0f) && r.batt==35);
    // A 14-byte 0x181A payload is neither ATC1441 (13) nor PVVX (15) — no decode
    SensorReading x; uint8_t xx[] = {0x11,0x16,0x1a,0x18, 0xa4,0xc1,0x38,0xaa,0xbb,0xcc,
                                     0x00,0xf0, 0x2d, 0x5d, 0x0b,0x54, 0x21, 0x00};
    CHECK(decodeAdvertisement(xx, sizeof xx, nullptr, x) == nullptr);
}

// ── BTHome v2 object sizes: the uint16 ids hiding in the binary-sensor block ─
static void test_bthome_objlen(void) {
    // CO2/TVOC/moisture are uint16 measurements at ids 0x12-0x14, inside the
    // 0x0F..0x2D binary-sensor block — the explicit table must win over it
    CHECK(bthomeObjLen(0x12) == 2 && bthomeObjLen(0x13) == 2 && bthomeObjLen(0x14) == 2);
    // Their binary-sensor neighbours stay one byte
    CHECK(bthomeObjLen(0x0f) == 1 && bthomeObjLen(0x11) == 1);
    CHECK(bthomeObjLen(0x15) == 1 && bthomeObjLen(0x2d) == 1);
    // Outside the block nothing changes: known uint8 id, unknown id
    CHECK(bthomeObjLen(0x2e) == 1 && bthomeObjLen(0xe0) == -1);
}

// ── BTHome v2 (0xFCD2): object order, packet id, Shelly BLU H&T objects ─────
static void test_bthome(void) {
    // Spec-ordered frame (ids ascending): batt 88, temp 23.45, hum 48.00
    SensorReading b; uint8_t bt[] = {0x0c,0x16,0xd2,0xfc, 0x40, 0x01,0x58, 0x02,0x29,0x09, 0x03,0xc0,0x12};
    CHECK(isType(bt, sizeof bt, "BTHome v2", b) && feq(b.temp,23.45f) && feq(b.hum,48.0f) && b.batt==88);
    // Out-of-spec object order still tolerated (temp, hum, batt)
    SensorReading o; uint8_t oo[] = {0x0c,0x16,0xd2,0xfc, 0x40, 0x02,0x29,0x09, 0x03,0xc0,0x12, 0x01,0x58};
    CHECK(isType(oo, sizeof oo, "BTHome v2", o) && feq(o.temp,23.45f) && o.batt==88);
    // Packet id (0x00) leads a spec-ordered payload — must be skipped, not abort
    SensorReading p; uint8_t pid[] = {0x0e,0x16,0xd2,0xfc, 0x40, 0x00,0x0b, 0x01,0x58, 0x02,0x29,0x09, 0x03,0xc0,0x12};
    CHECK(isType(pid, sizeof pid, "BTHome v2", p) && feq(p.temp,23.45f) && feq(p.hum,48.0f) && p.batt==88);
    // Shelly BLU H&T shape: pid + battery + humidity (0x2E, 1 %) + temp (0x45, 0.1 C)
    SensorReading s; uint8_t sh[] = {0x0d,0x16,0xd2,0xfc, 0x40, 0x00,0x4e, 0x01,0x64, 0x2e,0x2d, 0x45,0xea,0x00};
    CHECK(isType(sh, sizeof sh, "BTHome v2", s) && feq(s.temp,23.4f) && feq(s.hum,45.0f) && s.batt==100);
    // 0x45 temperature is signed: -5.5 C
    SensorReading n; uint8_t ng[] = {0x07,0x16,0xd2,0xfc, 0x40, 0x45,0xc9,0xff};
    CHECK(isType(ng, sizeof ng, "BTHome v2", n) && feq(n.temp,-5.5f));
    // Button event (0x3A) before the 0x45 temp is skipped via the size table
    SensorReading e; uint8_t ev[] = {0x09,0x16,0xd2,0xfc, 0x40, 0x3a,0x00, 0x45,0xea,0x00};
    CHECK(isType(ev, sizeof ev, "BTHome v2", e) && feq(e.temp,23.4f));
    // A uint16 CO2 object (0x12) between temp and humidity: read as one byte it
    // desynced the walk and every later object was lost — temp 22.00, hum 55 %
    SensorReading q; uint8_t qc[] = {0x0c,0x16,0xd2,0xfc, 0x40, 0x02,0x98,0x08, 0x12,0x20,0x03, 0x2e,0x37};
    CHECK(isType(qc, sizeof qc, "BTHome v2", q) && feq(q.temp,22.0f) && feq(q.hum,55.0f));
    // Worse than a lost field: CO2 600 ppm then 24.1 C used to desync into a
    // *plausible* -37.7 C, which passes validTemp and reaches the heat pump
    SensorReading w; uint8_t wt[] = {0x0a,0x16,0xd2,0xfc, 0x40, 0x12,0x58,0x02, 0x45,0xf1,0x00};
    CHECK(isType(wt, sizeof wt, "BTHome v2", w) && feq(w.temp,24.1f));
    // All three uint16 ids in one spec-ordered frame: pid, CO2, TVOC, moisture,
    // hum 45 %, temp 23.4 C — the trailing objects must survive the skips
    SensorReading m; uint8_t mx[] = {0x14,0x16,0xd2,0xfc, 0x40, 0x00,0x0b, 0x12,0x20,0x03, 0x13,0x2c,0x01,
                                     0x14,0x94,0x11, 0x2e,0x2d, 0x45,0xea,0x00};
    CHECK(isType(mx, sizeof mx, "BTHome v2", m) && feq(m.temp,23.4f) && feq(m.hum,45.0f));
    // Unknown object id after temp: keep what already decoded
    SensorReading u; uint8_t un[] = {0x0a,0x16,0xd2,0xfc, 0x40, 0x02,0x29,0x09, 0xe0,0x12,0x34};
    CHECK(isType(un, sizeof un, "BTHome v2", u) && feq(u.temp,23.45f));
    // Unknown object id before temp: size unknowable -> no decode
    SensorReading x; uint8_t ux[] = {0x09,0x16,0xd2,0xfc, 0x40, 0xe0,0x12, 0x02,0x29,0x09};
    CHECK(!isType(ux, sizeof ux, "BTHome v2", x) && std::isnan(x.temp));
    // No temperature object -> not identified; partial values must not leak out
    SensorReading h; uint8_t ho[] = {0x07,0x16,0xd2,0xfc, 0x40, 0x03,0xc0,0x12};
    CHECK(!isType(ho, sizeof ho, "BTHome v2", h) && std::isnan(h.hum));
    // Encrypted flag rejected
    SensorReading c; uint8_t enc[] = {0x0c,0x16,0xd2,0xfc, 0x41, 0x01,0x58, 0x02,0x29,0x09, 0x03,0xc0,0x12};
    CHECK(!isType(enc, sizeof enc, "BTHome v2", c));
    // Future BTHome version (device-info bits 5-7 != 2) rejected
    SensorReading v; uint8_t v3[] = {0x0c,0x16,0xd2,0xfc, 0x60, 0x01,0x58, 0x02,0x29,0x09, 0x03,0xc0,0x12};
    CHECK(!isType(v3, sizeof v3, "BTHome v2", v));
}

int main(void) {
    test_switchbot_temphum();
    test_switchbot_meterpro_split();
    test_switchbot_nonmeter_gate();
    test_switchbot_meter_single();
    test_govee();
    test_pvvx();
    test_atc1441();
    test_bthome_objlen();
    test_bthome();
    printf("ble_decoders: all %d checks passed\n", g_checks);
    return 0;
}
