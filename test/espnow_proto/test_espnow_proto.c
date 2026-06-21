#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "espnow_proto.h"

static void test_sizes(void) {
    assert(sizeof(struct espnow_state_pkt) == 71);
    assert(sizeof(struct espnow_cmd_pkt)   == 8);
    assert(sizeof(struct espnow_probe_pkt) == 2);
}

static void test_temp_celsius_roundtrip(void) {
    assert(espnow_c_to_dc(21.5f) == 215);
    assert(espnow_c_to_dc(0.0f)  == 0);
    assert(espnow_c_to_dc(-3.5f) == -35);
    assert(espnow_dc_to_c(215) == 21.5f);
}

static void test_display_celsius(void) {
    /* useF = false -> whole degrees C */
    assert(espnow_dc_to_display(215, false) == 22);   /* rounds 21.5 -> 22 */
    assert(espnow_dc_to_display(210, false) == 21);
    assert(espnow_display_to_dc(21, false) == 210);
    assert(espnow_display_to_dc(22, false) == 220);  /* on-grid whole degree */
}

/* ── Reference: the exact °F<->°C table from web/index.html (F_TABLE plus
 * cToFTable/fToCTable). The Dial's wire conversion MUST agree with this, so a
 * setpoint shown as N°F on the Dial reads back as the same N°F in the web UI
 * and on the unit. This is the convention the proto helpers are validated
 * against — keep it in sync with web/index.html. ─────────────────────────── */
static const float WEB_FTAB[] = {
    16.0f,16.5f,17.0f,17.5f,18.0f,18.5f,19.0f,20.0f,21.0f,21.5f,
    22.0f,22.5f,23.0f,23.5f,24.0f,24.5f,25.0f,25.5f,26.0f,26.5f,
    27.0f,27.5f,28.0f,28.5f,29.0f,29.5f,30.0f,30.5f,
};
#define WEB_FTAB_N ((int)(sizeof(WEB_FTAB)/sizeof(WEB_FTAB[0])))
static int web_c_to_f(float c) {            /* cToFTable(): nearest, low °F wins ties */
    int bestF = 61; float bestD = 999.0f;
    for (int i = 0; i < WEB_FTAB_N; i++) {
        float d = fabsf(WEB_FTAB[i] - c);
        if (d < bestD) { bestD = d; bestF = 61 + i; }
    }
    return bestF;
}
static float web_f_to_c(int f) {            /* fToCTable() */
    if (f < 61) f = 61;
    if (f > 88) f = 88;
    return WEB_FTAB[f - 61];
}

static void test_display_fahrenheit(void) {
    /* Mitsubishi table convention (matches web F_TABLE), NOT linear:
     * 22.0C displays as 71F, 22.5C as 72F. */
    assert(espnow_dc_to_display(220, true) == 71);
    assert(espnow_dc_to_display(225, true) == 72);
    /* 72F -> 22.5C -> 225 dc. (The bug sent 220/22.0C, which the unit/web
     * read back as 71F — hence "Dial 72 -> serin 71".) */
    assert(espnow_display_to_dc(72, true) == 225);
    assert(espnow_display_to_dc(70, true) == 215);  /* 70F -> 21.5C */
    /* Measured temps (room/outside) can fall outside the setpoint table;
     * those fall back to linear, matching web fmtReadingTemp(). */
    assert(espnow_dc_to_display(350, true) == 95);  /* 35.0C -> 95F (linear) */
    assert(espnow_dc_to_display(100, true) == 50);  /* 10.0C -> 50F (linear) */
}

/* Regression for the Dial<->serin °F mismatch (Dial 72F showed 71F on the
 * unit). Every °F the Dial can send must map to the same °C the web means by
 * that °F, read back as the same °F in the web UI, and round-trip on the Dial. */
static void test_fahrenheit_web_consistency(void) {
    for (int f = 61; f <= 88; f++) {
        int16_t dc = espnow_display_to_dc(f, true);
        assert(dc == (int16_t)lroundf(web_f_to_c(f) * 10.0f)); /* same °C as web */
        assert(web_c_to_f((float)dc / 10.0f) == f);            /* web reads back f */
        assert(espnow_dc_to_display(dc, true) == f);           /* Dial round-trips */
    }
    /* Exact cases from the bug report. */
    assert(espnow_dc_to_display(espnow_display_to_dc(72, true), true) == 72);
    assert(espnow_dc_to_display(espnow_display_to_dc(69, true), true) == 69);
    assert(espnow_dc_to_display(espnow_display_to_dc(68, true), true) == 68);
}

static void test_state_flags(void) {
    uint8_t f = espnow_make_state_flags(
        /*power*/true, /*cn105*/false, /*operating*/true, /*wifi*/true,
        /*hk*/false, /*useF*/true, /*outValid*/false, /*runValid*/true);
    assert(f & ESPNOW_SF_POWER);
    assert(!(f & ESPNOW_SF_CN105));
    assert(f & ESPNOW_SF_OPERATING);
    assert(f & ESPNOW_SF_WIFI);
    assert(!(f & ESPNOW_SF_HK));
    assert(f & ESPNOW_SF_USE_F);
    assert(!(f & ESPNOW_SF_OUT_VALID));
    assert(f & ESPNOW_SF_RUNTIME_VALID);
}

static void test_parse_mac(void) {
    uint8_t m[6];
    assert(espnow_parse_mac("A1:B2:C3:D4:E5:F6", m));
    assert(m[0]==0xA1 && m[1]==0xB2 && m[2]==0xC3 && m[3]==0xD4 && m[4]==0xE5 && m[5]==0xF6);
    assert(!espnow_parse_mac("A1:B2:C3:D4:E5", m));      /* too short */
    assert(!espnow_parse_mac("ZZ:B2:C3:D4:E5:F6", m));   /* bad hex */
}

static void test_parse_hex16(void) {
    uint8_t k[16];
    assert(espnow_parse_hex16("000102030405060708090a0b0c0d0e0f", k));
    for (int i = 0; i < 16; i++) assert(k[i] == i);
    assert(!espnow_parse_hex16("0001", k));               /* too short */
    assert(!espnow_parse_hex16("zz0102030405060708090a0b0c0d0e0f", k));
}

static void test_struct_wire_roundtrip(void) {
    struct espnow_state_pkt p;
    memset(&p, 0, sizeof(p));
    p.type = ESPNOW_PKT_STATE; p.version = ESPNOW_PROTO_VERSION;
    p.mode = 0x03; p.set_dc = 240; p.room_dc = 231;
    strcpy((char*)p.ssid, "homenet");
    uint8_t buf[sizeof(p)];
    memcpy(buf, &p, sizeof(p));            /* encode == raw copy (LE both ends) */
    struct espnow_state_pkt q;
    memcpy(&q, buf, sizeof(q));            /* decode */
    assert(q.type == ESPNOW_PKT_STATE && q.version == ESPNOW_PROTO_VERSION);
    assert(q.set_dc == 240 && q.room_dc == 231 && q.mode == 0x03);
    assert(strcmp((char*)q.ssid, "homenet") == 0);
}

static void test_pair_packets(void) {
    assert(sizeof(struct espnow_pair_req_pkt)  == 56);
    assert(sizeof(struct espnow_pair_resp_pkt) == 56);

    struct espnow_pair_req_pkt q;
    memset(&q, 0, sizeof(q));
    q.type = ESPNOW_PKT_PAIR_REQ; q.version = ESPNOW_PROTO_VERSION;
    for (int i = 0; i < 6; i++)  q.src_mac[i] = (uint8_t)(0x10 + i);
    for (int i = 0; i < 32; i++) q.pub[i]     = (uint8_t)i;

    uint8_t tr[40];
    assert(espnow_pair_req_transcript(&q, tr) == 40);
    assert(tr[0] == ESPNOW_PKT_PAIR_REQ && tr[1] == ESPNOW_PROTO_VERSION);
    assert(tr[2] == 0x10 && tr[7] == 0x15);          /* src_mac */
    assert(tr[8] == 0 && tr[39] == 31);              /* pub */

    struct espnow_pair_resp_pkt r;
    memset(&r, 0, sizeof(r));
    r.type = ESPNOW_PKT_PAIR_RESP; r.version = ESPNOW_PROTO_VERSION;
    for (int i = 0; i < 6; i++)  r.src_mac[i] = (uint8_t)(0x20 + i);
    for (int i = 0; i < 32; i++) r.pub[i]     = (uint8_t)(0x80 + i);
    uint8_t dial_pub[32];
    for (int i = 0; i < 32; i++) dial_pub[i] = (uint8_t)i;

    uint8_t tr2[72];
    assert(espnow_pair_resp_transcript(&r, dial_pub, tr2) == 72);
    assert(tr2[2] == 0x20 && tr2[8] == 0x80 && tr2[40] == 0); /* resp pub then dial_pub */
    assert(tr2[71] == 31);

    uint8_t up[32], dp[32], salt[64];
    for (int i = 0; i < 32; i++) { dp[i] = (uint8_t)i; up[i] = (uint8_t)(0x40 + i); }
    espnow_pair_salt(dp, up, salt);
    assert(salt[0] == 0 && salt[31] == 31 && salt[32] == 0x40 && salt[63] == 0x5F);
}

int main(void) {
    test_sizes();
    test_temp_celsius_roundtrip();
    test_display_celsius();
    test_display_fahrenheit();
    test_fahrenheit_web_consistency();
    test_state_flags();
    test_struct_wire_roundtrip();
    test_parse_mac();
    test_parse_hex16();
    test_pair_packets();
    printf("ALL TESTS PASSED\n");
    return 0;
}
