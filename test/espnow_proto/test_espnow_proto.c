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

static void test_display_fahrenheit(void) {
    /* 22.0C == 71.6F -> displays 72; back to C snapped to 0.5 grid */
    assert(espnow_dc_to_display(220, true) == 72);
    /* 72F -> 22.22C -> snap 0.5 -> 22.0C -> 220 dc */
    assert(espnow_display_to_dc(72, true) == 220);
    /* 70F -> 21.11C -> snap -> 21.0C -> 210 */
    assert(espnow_display_to_dc(70, true) == 210);
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
    test_state_flags();
    test_struct_wire_roundtrip();
    test_parse_mac();
    test_parse_hex16();
    test_pair_packets();
    printf("ALL TESTS PASSED\n");
    return 0;
}
