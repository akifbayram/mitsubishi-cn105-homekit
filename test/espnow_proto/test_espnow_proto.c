#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "espnow_proto.h"

static void test_sizes(void) {
    assert(sizeof(struct espnow_state_pkt) == 14);  /* v6: +reserved[2] */
    assert(sizeof(struct espnow_info_pkt)  == 101); /* v10: +hk_payload[24] */
    assert(sizeof(struct espnow_cmd_pkt)   == 12);  /* v6: +reserved[1] */
    assert(sizeof(struct espnow_probe_pkt) == 4);   /* v6: +reserved[1] */
    /* The compat floor must never exceed the current version. */
    assert(ESPNOW_PROTO_MIN_COMPAT <= ESPNOW_PROTO_VERSION);
    /* MIN_LEN stays pinned to the floor-era (v5) sizes. */
    assert(ESPNOW_STATE_MIN_LEN == 12 && ESPNOW_INFO_MIN_LEN == 75);
    assert(ESPNOW_CMD_MIN_LEN == 11 && ESPNOW_PROBE_MIN_LEN == 3);
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
        /*hk*/false, /*useF*/true, /*vane_config*/2);
    assert(f & ESPNOW_SF_POWER);
    assert(!(f & ESPNOW_SF_CN105));
    assert(f & ESPNOW_SF_OPERATING);
    assert(f & ESPNOW_SF_WIFI);
    assert(!(f & ESPNOW_SF_HK));
    assert(f & ESPNOW_SF_USE_F);
    assert(espnow_state_vanecfg(f) == 2);   /* vane_config packed in bits 6,7 */

    uint8_t i = espnow_make_info_flags(/*out*/false);
    assert(!(i & ESPNOW_IF_OUT_VALID));
    assert(espnow_make_info_flags(/*out*/true) & ESPNOW_IF_OUT_VALID);
}

/* Regression for the unit<->Dial pairing failure: the Dial bumped
 * ESPNOW_PROTO_VERSION to 4 and added a unit-wide units field to
 * espnow_cmd_pkt without porting the change back into the unit's copy of
 * this header, so the unit's onRecv version gate silently dropped every
 * Dial packet (including PAIR_REQ).
 *
 * Deliberate tripwire: this equality FAILS the moment ESPNOW_PROTO_VERSION
 * changes, forcing whoever bumps it to (a) sync both header copies and (b)
 * decide whether the change is additive (leave MIN_COMPAT) or breaking (raise
 * it). Update the constant below only after doing both. */
static void test_cmd_units_field(void) {
    assert(ESPNOW_PROTO_VERSION == 11);   /* v11: multi-zone IDENT */
    struct espnow_cmd_pkt c;
    memset(&c, 0, sizeof(c));
    c.type = ESPNOW_PKT_CMD; c.version = ESPNOW_PROTO_VERSION;
    c.mask = ESPNOW_CM_UNITS; c.use_f = 1;
    uint8_t buf[sizeof(c)];
    memcpy(buf, &c, sizeof(c));
    struct espnow_cmd_pkt d;
    memcpy(&d, buf, sizeof(d));
    assert(d.mask & ESPNOW_CM_UNITS);
    assert(d.use_f == 1);
}

/* Forward-compat contract: a future peer may append fields (bumping VERSION but
 * not MIN_COMPAT) and send a LONGER packet. The receiver copies only the prefix
 * it understands (memcpy sizeof(known_struct)) and ignores the tail — mirroring
 * the `len >= sizeof(...)` checks in onRecv(). The known fields must survive. */
static void test_forward_compat_prefix(void) {
    struct espnow_cmd_pkt c;
    memset(&c, 0, sizeof(c));
    c.type = ESPNOW_PKT_CMD; c.version = ESPNOW_PROTO_VERSION + 3; /* "newer" peer */
    c.mask = ESPNOW_CM_TEMP; c.set_dc = 235;

    /* Wire frame is longer than our struct (appended future fields = junk here). */
    uint8_t wire[sizeof(c) + 5];
    memset(wire, 0xAB, sizeof(wire));
    memcpy(wire, &c, sizeof(c));

    struct espnow_cmd_pkt got;
    espnow_decode_pkt(&got, sizeof(got), wire, (int)sizeof(wire));
    assert(got.type == ESPNOW_PKT_CMD);
    assert(got.version == ESPNOW_PROTO_VERSION + 3);
    assert(got.mask == ESPNOW_CM_TEMP && got.set_dc == 235);
    assert(got.reserved[0] == 0);            /* unknown tail did NOT leak in */
}

/* Backward-compat contract: a MIN_COMPAT-era (v5) peer sends SHORTER packets —
 * the structs grew reserved tails in v6. The receiver gates on the floor-era
 * ESPNOW_*_MIN_LEN and decodes with zero-fill + prefix copy, so the missing
 * tail reads as zeros. This is exactly what a v6 unit does with a v5 Dial's
 * 11-byte CMD and 3-byte PROBE. */
static void test_backward_compat_short(void) {
    /* Build a v5-shaped 11-byte CMD: today's layout minus the reserved tail. */
    struct espnow_cmd_pkt full;
    memset(&full, 0, sizeof(full));
    full.type = ESPNOW_PKT_CMD; full.version = 5;
    full.mask = ESPNOW_CM_TEMP | ESPNOW_CM_FAN; full.fan = 2; full.set_dc = 215;
    uint8_t wire[ESPNOW_CMD_MIN_LEN];
    memcpy(wire, &full, sizeof(wire));       /* truncate to the v5 size */

    assert((int)sizeof(wire) >= ESPNOW_CMD_MIN_LEN);   /* receiver's gate */
    struct espnow_cmd_pkt got;
    memset(&got, 0xEE, sizeof(got));         /* dirty struct: decode must clear */
    espnow_decode_pkt(&got, sizeof(got), wire, (int)sizeof(wire));
    assert(got.version == 5);
    assert(got.mask == (ESPNOW_CM_TEMP | ESPNOW_CM_FAN));
    assert(got.fan == 2 && got.set_dc == 215);
    assert(got.reserved[0] == 0);            /* absent tail zero-filled */
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
    p.mode = 0x03; p.set_dc = 240; p.room_dc = 231; p.error_code = 0x80;
    uint8_t buf[sizeof(p)];
    memcpy(buf, &p, sizeof(p));
    struct espnow_state_pkt q;
    memcpy(&q, buf, sizeof(q));
    assert(q.type == ESPNOW_PKT_STATE && q.version == ESPNOW_PROTO_VERSION);
    assert(q.set_dc == 240 && q.room_dc == 231 && q.mode == 0x03 && q.error_code == 0x80);
}

static void test_info_wire_roundtrip(void) {
    struct espnow_info_pkt p;
    memset(&p, 0, sizeof(p));
    p.type = ESPNOW_PKT_INFO; p.version = ESPNOW_PROTO_VERSION;
    p.iflags = espnow_make_info_flags(true);
    p.compressor_hz = 42; p.outside_dc = 175; p.sub_mode = 0x02; p.wifi_rssi = -58;
    strcpy((char*)p.ssid, "homenet");
    strcpy((char*)p.ip, "192.168.1.50");
    strcpy((char*)p.hk_code, "548-94-669");
    uint8_t buf[sizeof(p)];
    memcpy(buf, &p, sizeof(p));
    struct espnow_info_pkt q;
    memcpy(&q, buf, sizeof(q));
    assert(q.type == ESPNOW_PKT_INFO && q.compressor_hz == 42);
    assert(q.outside_dc == 175 && q.sub_mode == 0x02 && q.wifi_rssi == -58);
    assert(q.iflags & ESPNOW_IF_OUT_VALID);
    assert(strcmp((char*)q.ssid, "homenet") == 0);
    assert(strcmp((char*)q.ip, "192.168.1.50") == 0);
    assert(strcmp((char*)q.hk_code, "548-94-669") == 0);
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

static void test_wifi_pkts(void) {
    /* v7: Link OTA credential relay (new packet types; floor unchanged) */
    assert(ESPNOW_PKT_WIFI_REQ == 7 && ESPNOW_PKT_WIFI_RESP == 8);
    assert(sizeof(struct espnow_wifi_req_pkt)  == 4);
    assert(sizeof(struct espnow_wifi_resp_pkt) == 103);
    assert(ESPNOW_WIFI_REQ_MIN_LEN == 4 && ESPNOW_WIFI_RESP_MIN_LEN == 103);
    assert(ESPNOW_PROTO_MIN_COMPAT == 5);     /* additive change: floor must not move */
    /* tolerant decode zero-fills the tail beyond a short frame */
    uint8_t raw[4] = { ESPNOW_PKT_WIFI_RESP, ESPNOW_PROTO_VERSION, 1, 'A' };
    struct espnow_wifi_resp_pkt r;
    espnow_decode_pkt(&r, sizeof(r), raw, (int)sizeof(raw));
    assert(r.type == ESPNOW_PKT_WIFI_RESP && r.ok == 1);
    assert(r.ssid[0] == 'A' && r.ssid[1] == 0 && r.psk[0] == 0);
}

/* v8: STATE flags2 carries the remote-sensor low-battery bit. reserved[0] was
 * claimed as flags2 (sizeof unchanged), and a MIN_COMPAT-era 12-byte STATE
 * must decode with flags2 == 0 (zero-fill), i.e. no battery UI. */
static void test_state_flags2_sensor_batt(void) {
    struct espnow_state_pkt p;
    memset(&p, 0, sizeof(p));
    p.type = ESPNOW_PKT_STATE; p.version = ESPNOW_PROTO_VERSION;
    p.flags2 = ESPNOW_SF2_SENSOR_BATT_LOW;
    uint8_t buf[sizeof(p)];
    memcpy(buf, &p, sizeof(p));
    struct espnow_state_pkt q;
    memcpy(&q, buf, sizeof(q));
    assert(q.flags2 & ESPNOW_SF2_SENSOR_BATT_LOW);

    /* v5-shaped 12-byte STATE (pre-reserved-tail): flags2 must read 0. */
    struct espnow_state_pkt full;
    memset(&full, 0, sizeof(full));
    full.type = ESPNOW_PKT_STATE; full.version = 5;
    full.flags2 = ESPNOW_SF2_SENSOR_BATT_LOW;   /* would-be junk from a newer field */
    uint8_t wire[ESPNOW_STATE_MIN_LEN];
    memcpy(wire, &full, sizeof(wire));          /* truncate to v5 size (12) */
    assert((int)sizeof(wire) >= ESPNOW_STATE_MIN_LEN);
    struct espnow_state_pkt got;
    memset(&got, 0xEE, sizeof(got));
    espnow_decode_pkt(&got, sizeof(got), wire, (int)sizeof(wire));
    assert(got.flags2 == 0);                    /* absent tail zero-filled */
}

/* v8: INFO carries the raw sensor battery %, valid per IF_SENSORBATT_VALID.
 * A short v7-era 75-byte INFO decodes with the byte 0 and the valid bit clear. */
static void test_info_sensor_batt(void) {
    struct espnow_info_pkt p;
    memset(&p, 0, sizeof(p));
    p.type = ESPNOW_PKT_INFO; p.version = ESPNOW_PROTO_VERSION;
    p.iflags = ESPNOW_IF_SENSORBATT_VALID;
    p.sensor_batt_pct = 87;
    uint8_t buf[sizeof(p)];
    memcpy(buf, &p, sizeof(p));
    struct espnow_info_pkt q;
    memcpy(&q, buf, sizeof(q));
    assert(q.iflags & ESPNOW_IF_SENSORBATT_VALID);
    assert(q.sensor_batt_pct == 87);

    /* MIN_COMPAT-era 75-byte INFO: valid bit clear, byte zero-filled. */
    uint8_t wire[ESPNOW_INFO_MIN_LEN];
    memset(wire, 0, sizeof(wire));
    wire[0] = ESPNOW_PKT_INFO; wire[1] = 5;
    struct espnow_info_pkt got;
    memset(&got, 0xEE, sizeof(got));
    espnow_decode_pkt(&got, sizeof(got), wire, (int)sizeof(wire));
    assert(!(got.iflags & ESPNOW_IF_SENSORBATT_VALID));
    assert(got.sensor_batt_pct == 0);
}

static void test_diag_pkt(void) {
    /* v9: new pull-only unit->dial diagnostics packet. New at v9, so its
     * MIN_LEN == its sizeof (no shorter-era senders exist). */
    assert(ESPNOW_PROTO_VERSION == 11);   /* v11: multi-zone IDENT */
    assert(ESPNOW_PROTO_MIN_COMPAT == 5);          /* additive: floor unchanged */
    assert(ESPNOW_PKT_DIAG == 9);
    assert(sizeof(struct espnow_diag_pkt) == 56);
    assert(ESPNOW_DIAG_MIN_LEN == 56);

    struct espnow_diag_pkt p; memset(&p, 0, sizeof p);
    p.type = ESPNOW_PKT_DIAG; p.version = ESPNOW_PROTO_VERSION;
    p.dflags = ESPNOW_DF_RUNTIME_VALID | ESPNOW_DF_BLE_VALID | ESPNOW_DF_TEMP_SRC_REMOTE;
    strcpy((char*)p.fw_ver, "1.4.0");
    strcpy((char*)p.build_date, "Jul  5 2026");
    p.uptime_s = 123456; p.reset_reason = 1; p.wifi_channel = 6;
    p.auto_sub_mode = 2; p.runtime_h = 4321;
    p.ble_temp_dc = 215; p.ble_hum_pct = 55; p.ble_rssi = -60;

    /* exact-size decode is an identity */
    struct espnow_diag_pkt q;
    espnow_decode_pkt(&q, sizeof q, &p, (int)sizeof p);
    assert(memcmp(&p, &q, sizeof p) == 0);

    /* a longer future packet's unknown tail is ignored */
    uint8_t buf[64]; memset(buf, 0xAA, sizeof buf); memcpy(buf, &p, sizeof p);
    espnow_decode_pkt(&q, sizeof q, buf, (int)sizeof buf);
    assert(memcmp(&p, &q, sizeof p) == 0);
}

/* v10: dial-driven Wi-Fi provisioning packet set. All four travel ONLY
 * unicast between bonded peers (LMK-encrypted). New packet types = VERSION
 * bump only; the floor stays at 5. */
static void test_wifi_provisioning_pkts(void) {
    assert(ESPNOW_PROTO_VERSION == 11);   /* v11: multi-zone IDENT */
    assert(ESPNOW_PROTO_MIN_COMPAT == 5);           /* additive: floor must not move */
    assert(ESPNOW_PKT_WIFI_SCAN_REQ == 10 && ESPNOW_PKT_WIFI_SCAN_RESP == 11);
    assert(ESPNOW_PKT_WIFI_SET == 12 && ESPNOW_PKT_WIFI_STATUS == 13);
    assert(sizeof(struct espnow_wifi_scan_req_pkt) == 4);
    assert(sizeof(struct espnow_wifi_scan_item) == 35);
    assert(sizeof(struct espnow_wifi_scan_resp_pkt) == 6 + 6 * 35);  /* header + 6 items, <250 */
    assert(sizeof(struct espnow_wifi_set_pkt) == 102);
    assert(sizeof(struct espnow_wifi_status_pkt) == 22);
    assert(ESPNOW_WIFI_SCAN_REQ_MIN_LEN == 4 && ESPNOW_WIFI_SCAN_RESP_HDR_LEN == 6);
    assert(ESPNOW_WIFI_SET_MIN_LEN == 102 && ESPNOW_WIFI_STATUS_MIN_LEN == 22);
    assert(ESPNOW_WIFI_SCAN_MAX_ITEMS == 6);

    /* SCAN_RESP pages are wire-truncated: header + n_items*35 bytes. A partial
     * page decodes; items beyond the wire frame are zero-filled. */
    struct espnow_wifi_scan_resp_pkt r; memset(&r, 0, sizeof r);
    r.type = ESPNOW_PKT_WIFI_SCAN_RESP; r.version = ESPNOW_PROTO_VERSION;
    r.page = 0; r.n_pages = 1; r.n_items = 2;
    strcpy((char*)r.items[0].ssid, "homenet"); r.items[0].rssi = -48; r.items[0].secure = 1;
    strcpy((char*)r.items[1].ssid, "guest");   r.items[1].rssi = -71; r.items[1].secure = 0;
    int wire_len = ESPNOW_WIFI_SCAN_RESP_HDR_LEN + 2 * (int)sizeof(struct espnow_wifi_scan_item);
    struct espnow_wifi_scan_resp_pkt got; memset(&got, 0xEE, sizeof got);
    espnow_decode_pkt(&got, sizeof got, &r, wire_len);
    assert(got.page == 0 && got.n_pages == 1 && got.n_items == 2);
    assert(strcmp((char*)got.items[0].ssid, "homenet") == 0);
    assert(got.items[0].rssi == -48 && got.items[0].secure == 1);
    assert(strcmp((char*)got.items[1].ssid, "guest") == 0);
    assert(got.items[2].ssid[0] == 0);              /* beyond the wire: zero-filled */

    /* WIFI_SET carries ssid+psk exactly like the proven WIFI_RESP shapes. */
    struct espnow_wifi_set_pkt w; memset(&w, 0, sizeof w);
    w.type = ESPNOW_PKT_WIFI_SET; w.version = ESPNOW_PROTO_VERSION;
    strcpy((char*)w.ssid, "homenet"); strcpy((char*)w.psk, "hunter2!pass");
    struct espnow_wifi_set_pkt wg;
    espnow_decode_pkt(&wg, sizeof wg, &w, (int)sizeof w);
    assert(strcmp((char*)wg.ssid, "homenet") == 0);
    assert(strcmp((char*)wg.psk, "hunter2!pass") == 0);

    /* WIFI_STATUS roundtrip. */
    struct espnow_wifi_status_pkt s; memset(&s, 0, sizeof s);
    s.type = ESPNOW_PKT_WIFI_STATUS; s.version = ESPNOW_PROTO_VERSION;
    s.status = ESPNOW_WIFI_ST_CONNECTED; s.rssi = -55;
    strcpy((char*)s.ip, "10.0.0.23");
    struct espnow_wifi_status_pkt sg;
    espnow_decode_pkt(&sg, sizeof sg, &s, (int)sizeof s);
    assert(sg.status == ESPNOW_WIFI_ST_CONNECTED && sg.rssi == -55);
    assert(strcmp((char*)sg.ip, "10.0.0.23") == 0);
}

/* v10: STATE flags2 gains WIFI_PROVISIONED (spare bit — sizeof unchanged).
 * A MIN_COMPAT-era 12-byte STATE decodes with flags2 == 0, and the DIAL must
 * additionally version-gate (peer >= 10) before trusting the bit — a v9 unit
 * with creds also sends flags2 bit1 == 0. */
static void test_state_flags2_wifi_provisioned(void) {
    assert(ESPNOW_SF2_WIFI_PROVISIONED == (1u << 1));
    struct espnow_state_pkt p; memset(&p, 0, sizeof p);
    p.type = ESPNOW_PKT_STATE; p.version = ESPNOW_PROTO_VERSION;
    p.flags2 = ESPNOW_SF2_WIFI_PROVISIONED;
    struct espnow_state_pkt q;
    espnow_decode_pkt(&q, sizeof q, &p, (int)sizeof p);
    assert(q.flags2 & ESPNOW_SF2_WIFI_PROVISIONED);
    /* v5-shaped 12-byte frame: flags2 zero-filled. */
    uint8_t wire[ESPNOW_STATE_MIN_LEN];
    memcpy(wire, &p, sizeof wire);
    struct espnow_state_pkt got; memset(&got, 0xEE, sizeof got);
    espnow_decode_pkt(&got, sizeof got, wire, (int)sizeof wire);
    assert(got.flags2 == 0);
}

/* v10: INFO appends hk_payload[24] (the X-HM:// HomeKit setup URI) after
 * reserved[] — sizeof grows 77 -> 101, MIN_LEN stays 75. A v9-era 77-byte
 * INFO decodes with hk_payload empty (dial falls back to code-only). */
static void test_info_hk_payload(void) {
    assert(sizeof(struct espnow_info_pkt) == 101);
    assert(ESPNOW_INFO_MIN_LEN == 75);
    struct espnow_info_pkt p; memset(&p, 0, sizeof p);
    p.type = ESPNOW_PKT_INFO; p.version = ESPNOW_PROTO_VERSION;
    strcpy((char*)p.hk_payload, "X-HM://00KFPZQT3MCAC");
    struct espnow_info_pkt q;
    espnow_decode_pkt(&q, sizeof q, &p, (int)sizeof p);
    assert(strcmp((char*)q.hk_payload, "X-HM://00KFPZQT3MCAC") == 0);
    /* v9-era 77-byte frame: hk_payload zero-filled. */
    uint8_t wire[77]; memset(wire, 0, sizeof wire);
    wire[0] = ESPNOW_PKT_INFO; wire[1] = 9;
    struct espnow_info_pkt got; memset(&got, 0xEE, sizeof got);
    espnow_decode_pkt(&got, sizeof got, wire, (int)sizeof wire);
    assert(got.hk_payload[0] == 0);
}

static void test_ident_pkt(void) {
    /* v11: PROBE claims reserved[0] as want_ident — sizeof unchanged */
    assert(sizeof(struct espnow_probe_pkt) == 4);
    struct espnow_probe_pkt pr = { ESPNOW_PKT_PROBE, ESPNOW_PROTO_VERSION, 1, 1 };
    assert(((const uint8_t *)&pr)[3] == 1);          /* want_ident is byte 3 */
    /* a v<=10 peer's short (3 B) probe decodes want_ident = 0 */
    uint8_t old[3] = { ESPNOW_PKT_PROBE, 10, 1 };
    struct espnow_probe_pkt d;
    espnow_decode_pkt(&d, sizeof d, old, sizeof old);
    assert(d.want_info == 1 && d.want_ident == 0);

    /* IDENT: new packet type, unit -> dial, pull-gated by want_ident */
    assert(ESPNOW_PKT_IDENT == 14);
    assert(sizeof(struct espnow_ident_pkt) == 36);
    assert(ESPNOW_IDENT_MIN_LEN == 36);
    struct espnow_ident_pkt p; memset(&p, 0, sizeof p);
    p.type = ESPNOW_PKT_IDENT; p.version = ESPNOW_PROTO_VERSION;
    strcpy((char *)p.name, "Living Room");
    struct espnow_ident_pkt q;
    espnow_decode_pkt(&q, sizeof q, &p, sizeof p);
    assert(q.type == ESPNOW_PKT_IDENT && !strcmp((char *)q.name, "Living Room"));

    /* additive: the compat floor must not move */
    assert(ESPNOW_PROTO_MIN_COMPAT == 5);
}

int main(void) {
    test_sizes();
    test_temp_celsius_roundtrip();
    test_display_celsius();
    test_display_fahrenheit();
    test_fahrenheit_web_consistency();
    test_state_flags();
    test_state_flags2_sensor_batt();
    test_info_sensor_batt();
    test_cmd_units_field();
    test_forward_compat_prefix();
    test_backward_compat_short();
    test_struct_wire_roundtrip();
    test_info_wire_roundtrip();
    test_parse_mac();
    test_parse_hex16();
    test_pair_packets();
    test_wifi_pkts();
    test_diag_pkt();
    test_wifi_provisioning_pkts();
    test_state_flags2_wifi_provisioned();
    test_info_hk_payload();
    test_ident_pkt();
    printf("ALL TESTS PASSED\n");
    return 0;
}
