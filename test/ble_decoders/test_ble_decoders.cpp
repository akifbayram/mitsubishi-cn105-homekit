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
static void test_govee(void) {
    // combined value 250500 (0x03d284) -> 25.0 C / 50.0 %
    SensorReading v3; uint8_t g3[] = {0x08,0xff,0x88,0xec,0x00,0x03,0xd2,0x84,0x64};
    CHECK(isType(g3, sizeof g3, "Govee V3", v3) && feq(v3.temp,25.0f) && feq(v3.hum,50.0f) && v3.batt==100);
    // V2 little-endian: 22.55 C / 45.10 %
    SensorReading v2; uint8_t g2[] = {0x09,0xff,0x88,0xec,0x00,0xcf,0x08,0x9e,0x11,0x5a};
    CHECK(isType(g2, sizeof g2, "Govee V2", v2) && feq(v2.temp,22.55f) && feq(v2.hum,45.10f) && v2.batt==90);
    // V1 (company 0x0001), combined @4
    SensorReading v1; uint8_t g1[] = {0x09,0xff,0x01,0x00,0x00,0x00,0x03,0xd2,0x84,0x64};
    CHECK(isType(g1, sizeof g1, "Govee V1", v1) && feq(v1.temp,25.0f) && feq(v1.hum,50.0f) && v1.batt==100);
}

// ── PVVX (0x181A) and BTHome v2 (0xFCD2) ────────────────────────────────────
static void test_pvvx_bthome(void) {
    // PVVX: temp 21.00, hum 55.00, batt 95
    SensorReading p; uint8_t pv[] = {0x10,0x16,0x1a,0x18, 0xaa,0xbb,0xcc,0xdd,0xee,0xff,
                                     0x34,0x08,0x7c,0x15,0x00,0x00,0x5f};
    CHECK(isType(pv, sizeof pv, "PVVX", p) && feq(p.temp,21.0f) && feq(p.hum,55.0f) && p.batt==95);
    // BTHome v2: temp 23.45, hum 48.00, batt 88
    SensorReading b; uint8_t bt[] = {0x0c,0x16,0xd2,0xfc, 0x40,0x02,0x29,0x09,0x03,0xc0,0x12,0x01,0x58};
    CHECK(isType(bt, sizeof bt, "BTHome v2", b) && feq(b.temp,23.45f) && feq(b.hum,48.0f) && b.batt==88);
}

int main(void) {
    test_switchbot_temphum();
    test_switchbot_meterpro_split();
    test_switchbot_nonmeter_gate();
    test_switchbot_meter_single();
    test_govee();
    test_pvvx_bthome();
    printf("ble_decoders: all %d checks passed\n", g_checks);
    return 0;
}
