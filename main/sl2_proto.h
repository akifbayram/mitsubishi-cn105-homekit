/*
 * sl2_proto.h — Serin Link protocol wire format.
 *
 * Dependency-free C (stdint/stdbool/string/math only): shared byte-identical
 * between every controller adapter and the Dial firmware. Little-endian packed
 * structs ARE the wire format (encode/decode == memcpy). Spec:
 * docs/serin-link-wire-spec.md (this repo).
 *
 * Growth discipline: additive changes only —
 * claim a spare flag bit, then a reserved[] byte, then append after reserved[].
 * Receivers gate on floor-era *_MIN_LEN (never sizeof) and decode tolerantly
 * with sl2_decode_pkt(). Never insert or resize an existing field.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL2_PROTO_VERSION    2
#define SL2_PROTO_MIN_COMPAT 1

/* esp_now_set_pmk() input: a documented PUBLIC constant (16 bytes). It only
 * wraps LMKs inside the radio driver; it is not a secret and not a trust
 * anchor — pairing trust comes from Ed25519 signatures + TOFU pinning. */
#define SL2_ESPNOW_PMK "serin-link-open\0"   /* 15 chars + NUL = 16 B */

enum sl2_pkt_type {
    SL2_PKT_STATE     = 1,   /* ctrl -> dial, encrypted */
    SL2_PKT_CMD       = 2,   /* dial -> ctrl, encrypted */
    SL2_PKT_PROBE     = 3,   /* dial -> ctrl, encrypted */
    SL2_PKT_PAIR_REQ  = 4,   /* dial -> broadcast, signed plaintext */
    SL2_PKT_PAIR_RESP = 5,   /* ctrl -> broadcast, signed plaintext */
    SL2_PKT_CAPS      = 6,   /* ctrl -> dial, encrypted, pull (WANT_CAPS) */
    SL2_PKT_INFO      = 7,   /* ctrl -> dial, encrypted, pull (WANT_INFO) */
    SL2_PKT_WIFI_REQ  = 8,   /* dial -> ctrl, encrypted (Link OTA) */
    SL2_PKT_WIFI_RESP = 9,   /* ctrl -> dial, encrypted (Link OTA) */
    SL2_PKT_WIFI_SETUP = 10, /* dial -> ctrl, encrypted (raise setup AP) */
    SL2_PKT_DIAL_INFO  = 11, /* dial -> ctrl, encrypted (dial identity) */
    /* 12..127 reserved for core growth; 128..255 experiments, never shipped */
};

/* ── semantic HVAC model ──────────────────────────────────────────────── */

enum sl2_mode {                 /* superset of ESPHome ClimateMode / HomeKit */
    SL2_MODE_OFF       = 0,
    SL2_MODE_HEAT      = 1,
    SL2_MODE_COOL      = 2,
    SL2_MODE_HEAT_COOL = 3,     /* dual setpoint: set_low_dc / set_high_dc */
    SL2_MODE_AUTO      = 4,     /* single-setpoint vendor auto */
    SL2_MODE_DRY       = 5,
    SL2_MODE_FAN_ONLY  = 6,
    /* 7..15 reserved — CAPS.modes mask is u16 */
};

enum sl2_action {               /* what the equipment is DOING right now */
    SL2_ACT_UNKNOWN = 0, SL2_ACT_IDLE    = 1, SL2_ACT_HEATING = 2,
    SL2_ACT_COOLING = 3, SL2_ACT_DRYING  = 4, SL2_ACT_FAN     = 5,
    SL2_ACT_DEFROST = 6, SL2_ACT_PREHEAT = 7, SL2_ACT_STANDBY = 8,
};

enum sl2_preset {               /* ESPHome preset superset; 0 = none */
    SL2_PRESET_NONE = 0, SL2_PRESET_ECO     = 1, SL2_PRESET_AWAY  = 2,
    SL2_PRESET_BOOST = 3, SL2_PRESET_COMFORT = 4, SL2_PRESET_HOME = 5,
    SL2_PRESET_SLEEP = 6, SL2_PRESET_ACTIVITY = 7,
    /* 8..15 reserved — CAPS.presets mask is u16 */
};

/* fan: 0 = auto, 1..100 = percent (dial sends canonical detent percents,
 * controller quantises to its own steps). vane per axis: 0 = auto,
 * 1..n_pos = positions (1 = most horizontal/left), 255 = swing. */
#define SL2_FAN_AUTO   0
#define SL2_VANE_AUTO  0
#define SL2_VANE_SWING 255

#define SL2_DC_NA      ((int16_t)0x7FFF)   /* "no value" for optional temps */
#define SL2_HUM_NA     ((uint8_t)0xFF)     /* "no value" for humidity fields */

/* ── STATE (ctrl -> dial) ─────────────────────────────────────────────── */

enum {  /* sl2_state_pkt.flags */
    SL2_SF_HVAC_LINK    = 1u << 0,   /* controller <-> heat pump link up */
    SL2_SF_WIFI         = 1u << 1,   /* controller STA associated */
    SL2_SF_USE_F        = 1u << 2,   /* display pref, per controller */
    SL2_SF_WIFI_PROVISIONED = 1u << 3, /* controller has stored STA creds
                                        * (drives the dial's Wi-Fi setup face;
                                        * always-on for YAML-provisioned fw) */
    SL2_SF_SETUP_AP     = 1u << 4,   /* recovery/setup hotspot currently active */
    SL2_SF_WIFI_ERR     = 1u << 5,   /* last credential change failed to join;
                                      * latched until the next successful join
                                      * or the next setup window (see spec 10b) */
    /* bits 6-7 spare */
};
enum {  /* sl2_state_pkt.flags2 */
    SL2_SF2_SENSOR_BATT_LOW = 1u << 0,
    /* bits 1-7 spare */
};

struct __attribute__((packed)) sl2_state_pkt {
    uint8_t  type;          /* SL2_PKT_STATE */
    uint8_t  version;       /* SL2_PROTO_VERSION */
    uint8_t  caps_seq;      /* bumps when CAPS content changes -> dial re-pulls */
    uint8_t  flags;         /* SL2_SF_* */
    uint8_t  mode;          /* enum sl2_mode */
    uint8_t  action;        /* enum sl2_action */
    uint8_t  fan;           /* 0=auto, 1..100 */
    uint8_t  vane_v;        /* 0=auto, 1..n, 255=swing; 0 if axis absent */
    uint8_t  vane_h;
    uint8_t  preset;        /* enum sl2_preset; 0 = none */
    uint16_t fault;         /* 0 = normal; else vendor fault code */
    int16_t  room_dc;       /* deci-C */
    int16_t  set_dc;        /* single-setpoint modes */
    int16_t  set_low_dc;    /* HEAT_COOL band; SL2_DC_NA when n/a */
    int16_t  set_high_dc;   /* HEAT_COOL band; SL2_DC_NA when n/a */
    uint8_t  room_hum_pct;  /* 0..100; SL2_HUM_NA = not reported */
    uint8_t  hum_set_pct;   /* 0..100; SL2_HUM_NA = n/a */
    uint8_t  flags2;        /* SL2_SF2_* */
    uint8_t  reserved[1];   /* senders zero-fill, receivers ignore */
    uint16_t epoch;         /* random per-boot replay token; 0 = sender has no
                             * epoch support. Dials echo the latest value in
                             * CMD/WIFI_SETUP (spec 3b). */
};
#define SL2_STATE_MIN_LEN 26

/* ── CMD (dial -> ctrl) ───────────────────────────────────────────────── */

enum {  /* sl2_cmd_pkt.mask */
    SL2_CM_MODE      = 1u << 0,
    SL2_CM_TEMP      = 1u << 1,
    SL2_CM_TEMP_BAND = 1u << 2,
    SL2_CM_FAN       = 1u << 3,
    SL2_CM_VANEV     = 1u << 4,
    SL2_CM_VANEH     = 1u << 5,
    SL2_CM_PRESET    = 1u << 6,
    SL2_CM_HUM       = 1u << 7,
    SL2_CM_UNITS     = 1u << 8,
    /* bits 9-15 spare */
};

struct __attribute__((packed)) sl2_cmd_pkt {
    uint8_t  type;          /* SL2_PKT_CMD */
    uint8_t  version;       /* SL2_PROTO_VERSION */
    uint16_t mask;          /* SL2_CM_*; apply masked fields only */
    uint8_t  mode;
    uint8_t  fan;
    uint8_t  vane_v;
    uint8_t  vane_h;
    uint8_t  preset;
    int16_t  set_dc;
    int16_t  set_low_dc;
    int16_t  set_high_dc;
    uint8_t  hum_set_pct;
    uint8_t  use_f;         /* SL2_CM_UNITS: 0/1 display pref */
    uint16_t epoch;         /* echo of the latest STATE.epoch; 0 = legacy dial.
                             * Odd wire offset (17) — packed access / memcpy
                             * only, like every field here. */
};
#define SL2_CMD_MIN_LEN 19

/* ── PROBE (dial -> ctrl) ─────────────────────────────────────────────── */

enum {  /* sl2_probe_pkt.want */
    SL2_WANT_INFO  = 1u << 0,
    SL2_WANT_CAPS  = 1u << 1,
    SL2_WANT_STATE = 1u << 2,  /* dial lacks a fresh STATE (zone switch/resync).
                                * Without this bit a controller that already
                                * considers the dial live (kept so by background
                                * keepalive probes) has no reason to send STATE
                                * before its 10 s heartbeat — the dial's sync
                                * watchdog then misreads the wait as link loss.
                                * Unknown bits are ignored, so mixed versions
                                * degrade to the old heartbeat-paced behavior. */
    /* bits 3-7 spare */
};

struct __attribute__((packed)) sl2_probe_pkt {
    uint8_t type;           /* SL2_PKT_PROBE */
    uint8_t version;        /* SL2_PROTO_VERSION */
    uint8_t want;           /* SL2_WANT_* pull gates */
    uint8_t reserved[1];    /* senders zero-fill, receivers ignore */
};
#define SL2_PROBE_MIN_LEN 3   /* want must be present; reserved may be absent */

/* ── Pairing (broadcast, plaintext, Ed25519-signed) ───────────────────── */

struct __attribute__((packed)) sl2_pair_req_pkt {
    uint8_t type;           /* SL2_PKT_PAIR_REQ */
    uint8_t version;        /* SL2_PROTO_VERSION */
    uint8_t src_mac[6];     /* dial STA MAC */
    uint8_t eph_pub[32];    /* fresh X25519 ephemeral, per attempt */
    uint8_t id_pub[32];     /* dial long-lived Ed25519 identity */
    uint8_t sig[64];        /* Ed25519(dial_id_priv, req transcript) */
    uint8_t reserved[1];
};

struct __attribute__((packed)) sl2_pair_resp_pkt {
    uint8_t type;           /* SL2_PKT_PAIR_RESP */
    uint8_t version;        /* SL2_PROTO_VERSION */
    uint8_t src_mac[6];     /* controller STA MAC */
    uint8_t eph_pub[32];    /* fresh X25519 ephemeral */
    uint8_t id_pub[32];     /* controller long-lived Ed25519 identity */
    uint8_t sig[64];        /* Ed25519(ctrl_id_priv, resp transcript) */
    uint8_t channel;        /* controller's live channel (1..13; 0 = unknown).
                             * Broadcasts bleed across adjacent channels at
                             * close range, so the sweep can catch this RESP
                             * one channel off — unicast then never ACKs. The
                             * dial retunes here before its confirm probes.
                             * SIGNED (in the transcript): a relayed copy with
                             * a doctored channel fails verification. */
};
#define SL2_PAIR_MIN_LEN 136   /* through sig[]; channel may be absent */

/* Signed transcripts, domain-separated. Layouts are wire-frozen. */
#define SL2_REQ_TRANSCRIPT_LEN  (8 + 2 + 6 + 32 + 32)            /* 80 */
#define SL2_RESP_TRANSCRIPT_LEN (9 + 2 + 6 + 32 + 32 + 32 + 1)   /* 114 */

static inline size_t sl2_pair_req_transcript(const struct sl2_pair_req_pkt *p,
                                             uint8_t out[SL2_REQ_TRANSCRIPT_LEN]) {
    memcpy(out, "SLv2-req", 8);
    out[8] = p->type; out[9] = p->version;
    memcpy(out + 10, p->src_mac, 6);
    memcpy(out + 16, p->eph_pub, 32);
    memcpy(out + 48, p->id_pub, 32);
    return SL2_REQ_TRANSCRIPT_LEN;
}

/* dial_eph_pub binds the response to THIS request. */
static inline size_t sl2_pair_resp_transcript(const struct sl2_pair_resp_pkt *p,
                                              const uint8_t dial_eph_pub[32],
                                              uint8_t out[SL2_RESP_TRANSCRIPT_LEN]) {
    memcpy(out, "SLv2-resp", 9);
    out[9] = p->type; out[10] = p->version;
    memcpy(out + 11, p->src_mac, 6);
    memcpy(out + 17, p->eph_pub, 32);
    memcpy(out + 49, p->id_pub, 32);
    memcpy(out + 81, dial_eph_pub, 32);
    out[113] = p->channel;
    return SL2_RESP_TRANSCRIPT_LEN;
}

/* HKDF inputs for the LMK — same on both ends. */
#define SL2_LMK_INFO      "serin-link-v2-lmk"
#define SL2_LMK_INFO_LEN  17
static inline void sl2_lmk_salt(const uint8_t dial_eph_pub[32],
                                const uint8_t ctrl_eph_pub[32], uint8_t out[64]) {
    memcpy(out, dial_eph_pub, 32);
    memcpy(out + 32, ctrl_eph_pub, 32);
}

/* ── CAPS (ctrl -> dial, pull via WANT_CAPS) ──────────────────────────── */

enum {  /* sl2_caps_pkt.caps_flags — control capabilities */
    SL2_CF_HUM_CTRL = 1u << 0,   /* accepts SL2_CM_HUM */
    /* bits 1-7 spare */
};
enum {  /* sl2_caps_pkt.features — telemetry/services the controller offers */
    SL2_FEAT_WIFI_INFO      = 1u << 0,
    SL2_FEAT_HOMEKIT        = 1u << 1,
    SL2_FEAT_OUTSIDE_T      = 1u << 2,
    SL2_FEAT_COMPRESSOR     = 1u << 3,
    SL2_FEAT_SENSOR_BATT    = 1u << 4,
    SL2_FEAT_FW_INFO        = 1u << 5,
    SL2_FEAT_RUNTIME        = 1u << 6,
    SL2_FEAT_LINK_OTA_CREDS = 1u << 7,
    SL2_FEAT_ENERGY         = 1u << 8,
    SL2_FEAT_WIFI_SETUP     = 1u << 9,  /* accepts WIFI_SETUP (on-demand setup AP) */
    /* bits 10-15 spare */
};

/* vane axis descriptor byte: low nibble = n_pos (0 = axis absent),
 * bit4 = supports auto, bit5 = supports swing. */
#define SL2_VANECAP(n_pos, has_auto, has_swing) \
    (uint8_t)(((n_pos) & 0x0F) | ((has_auto) ? 0x10 : 0) | ((has_swing) ? 0x20 : 0))
static inline uint8_t sl2_vanecap_npos(uint8_t v)      { return v & 0x0F; }
static inline bool    sl2_vanecap_has_auto(uint8_t v)  { return (v & 0x10) != 0; }
static inline bool    sl2_vanecap_has_swing(uint8_t v) { return (v & 0x20) != 0; }

struct __attribute__((packed)) sl2_caps_pkt {
    uint8_t  type;           /* SL2_PKT_CAPS */
    uint8_t  version;        /* SL2_PROTO_VERSION */
    uint8_t  caps_seq;       /* generation; STATE echoes it */
    uint8_t  caps_flags;     /* SL2_CF_* */
    uint16_t modes;          /* bit N = sl2_mode N supported (incl. OFF) */
    uint16_t presets;        /* bit N = sl2_preset N supported (bit 0 unused) */
    uint8_t  fan_steps;      /* 0 = fan not controllable; N = discrete steps */
    uint8_t  fan_flags;      /* bit0 = has auto; bits 1-7 spare */
    uint8_t  vane_v;         /* SL2_VANECAP() */
    uint8_t  vane_h;
    int16_t  set_min_dc;     /* setpoint range (also bounds the band) */
    int16_t  set_max_dc;
    uint8_t  set_step_dc;    /* deci-C: 5 = 0.5C, 10 = 1C */
    uint8_t  ftab_id;        /* 0 = linear, 1 = Mitsubishi 61-88F table */
    uint8_t  band_min_gap_dc;/* HEAT_COOL: min high-low separation; 0 = none */
    uint8_t  hum_step_pct;   /* target-humidity step; 0 = n/a */
    uint16_t features;       /* SL2_FEAT_* */
    char     name[32];       /* zone name, NUL-terminated; empty = unnamed */
    uint8_t  reserved[14];   /* senders zero-fill, receivers ignore */
};
#define SL2_CAPS_MIN_LEN 54   /* through name[]; reserved may be absent */
#define SL2_FAN_HAS_AUTO (1u << 0)

/* ── INFO (ctrl -> dial, pull via WANT_INFO): TLV stream ──────────────── */

struct __attribute__((packed)) sl2_info_pkt {
    uint8_t type;            /* SL2_PKT_INFO */
    uint8_t version;         /* SL2_PROTO_VERSION */
    uint8_t reserved[2];
    /* then TLVs back-to-back: u8 t; u8 l; u8 v[l]; ... */
};
#define SL2_INFO_HDR_LEN 4

enum sl2_tlv_type {          /* 0x01..0x7F core; 0x80..0xFF vendor-specific */
    SL2_TLV_WIFI_INFO   = 0x01,  /* i8 rssi; u8 channel; str ssid; str ip (NUL-joined) */
    SL2_TLV_HOMEKIT     = 0x02,  /* u8 paired; str code; str payload (NUL-joined) */
    SL2_TLV_OUTSIDE_T   = 0x03,  /* i16 outside_dc */
    SL2_TLV_COMPRESSOR  = 0x04,  /* u8 hz; u8 stage; u8 vendor_sub; u8 vendor_auto_sub */
    SL2_TLV_SENSOR_BATT = 0x05,  /* u8 pct */
    SL2_TLV_FW_INFO     = 0x06,  /* str version; str build_date (NUL-joined) */
    SL2_TLV_RUNTIME     = 0x07,  /* u32 hours */
    SL2_TLV_SYS         = 0x08,  /* u32 uptime_s; u8 reset_reason */
    SL2_TLV_ENERGY      = 0x09,  /* u16 input_w (0xFFFF n/a); u32 wh_total (0xFFFFFFFF n/a) */
    SL2_TLV_DIAL_CERT   = 0x0A,  /* DIAL_INFO tail only: raw 112 B link_cert
                                  * (Serin-signed device identity); absent on
                                  * unprovisioned dials */
};

/* Append one TLV. Returns false (and writes nothing) if it doesn't fit. */
static inline bool sl2_tlv_put(uint8_t *buf, size_t cap, size_t *off,
                               uint8_t t, const void *v, uint8_t l) {
    if (*off + 2 + (size_t)l > cap) return false;
    buf[*off] = t; buf[*off + 1] = l;
    if (l) memcpy(buf + *off + 2, v, l);
    *off += 2 + (size_t)l;
    return true;
}

/* Iterate TLVs in buf[0..len). Start with *off = 0. Returns false at end or
 * on a truncated TLV (which also ends iteration — never reads past len). */
static inline bool sl2_tlv_next(const uint8_t *buf, size_t len, size_t *off,
                                uint8_t *t, uint8_t *l, const uint8_t **v) {
    if (*off + 2 > len) return false;
    uint8_t tt = buf[*off], ll = buf[*off + 1];
    if (*off + 2 + (size_t)ll > len) return false;
    *t = tt; *l = ll; *v = buf + *off + 2;
    *off += 2 + (size_t)ll;
    return true;
}

/* ── Link OTA credential relay ────────────────────────────────────────── */

struct __attribute__((packed)) sl2_wifi_req_pkt {
    uint8_t type;            /* SL2_PKT_WIFI_REQ */
    uint8_t version;         /* SL2_PROTO_VERSION */
    uint8_t reserved[2];
};
#define SL2_WIFI_REQ_MIN_LEN 4

struct __attribute__((packed)) sl2_wifi_resp_pkt {
    uint8_t type;            /* SL2_PKT_WIFI_RESP */
    uint8_t version;         /* SL2_PROTO_VERSION */
    uint8_t ok;              /* 0 = no creds stored */
    char    ssid[33];        /* NUL-terminated */
    char    psk[65];         /* NUL-terminated; empty = open network */
    uint8_t reserved[2];
};
#define SL2_WIFI_RESP_MIN_LEN 103

/* ── dial-initiated Wi-Fi setup ───────────────────────────────────────── */

/* Ask the controller to raise its recovery/setup hotspot NOW (the dial's
 * change-network flow). No response packet — the controller reports the
 * hotspot via SL2_SF_SETUP_AP in STATE, and a flag flip re-sends STATE within
 * SL2_STATE_MIN_INTERVAL_MS. Senders re-fire ~1 Hz until they see the flag
 * (ESP-NOW is lossy); receivers must treat it as idempotent. Gated on
 * SL2_FEAT_WIFI_SETUP in CAPS. */
struct __attribute__((packed)) sl2_wifi_setup_pkt {
    uint8_t  type;           /* SL2_PKT_WIFI_SETUP */
    uint8_t  version;        /* SL2_PROTO_VERSION */
    uint16_t epoch;          /* echo of the latest STATE.epoch; 0 = legacy dial */
};
#define SL2_WIFI_SETUP_MIN_LEN 4

/* ── DIAL_INFO (dial -> ctrl, encrypted, push) ────────────────────────────
 * The dial reports its own identity so the unit's UI can show which Link is
 * paired. Sent on connect, on caps_seq change, and ~every 60 s. model/fw are
 * dial-authored branded strings; caps_seq is the dial's currently-applied
 * generation (unit compares against its own to show a "syncing" hint).
 *
 * Bytes past the fixed struct are an optional TLV tail (same framing as
 * INFO). SL2's tolerant decode copies min(len, sizeof) — floor-era
 * receivers drop the tail unread, so appending TLVs is compatible by
 * construction. Today: SL2_TLV_DIAL_CERT. */
struct __attribute__((packed)) sl2_dial_info_pkt {
    uint8_t type;            /* SL2_PKT_DIAL_INFO */
    uint8_t version;         /* SL2_PROTO_VERSION */
    uint8_t caps_seq;        /* the caps_seq the dial currently has applied */
    char    model[24];       /* branded model, NUL-terminated */
    char    fw[16];          /* firmware version, NUL-terminated */
};
#define SL2_DIAL_INFO_MIN_LEN 3   /* through caps_seq; model/fw may be absent */

/* ── sizeof guards — every vendored copy must agree ───────────────────── */
#define SL2_STATIC_ASSERT(c, m) typedef char sl2_sa_##m[(c) ? 1 : -1]
SL2_STATIC_ASSERT(sizeof(struct sl2_state_pkt)     == 26,  state_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_cmd_pkt)       == 19,  cmd_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_probe_pkt)     == 4,   probe_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_pair_req_pkt)  == 137, pair_req_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_pair_resp_pkt) == 137, pair_resp_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_caps_pkt)      == 68,  caps_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_info_pkt)      == 4,   info_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_wifi_req_pkt)  == 4,   wifi_req_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_wifi_resp_pkt) == 103, wifi_resp_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_wifi_setup_pkt) == 4,  wifi_setup_size);
SL2_STATIC_ASSERT(sizeof(struct sl2_dial_info_pkt) == 43,  dial_info_size);
SL2_STATIC_ASSERT(SL2_DIAL_INFO_MIN_LEN <= (int)sizeof(struct sl2_dial_info_pkt), dial_info_minlen);
SL2_STATIC_ASSERT(SL2_STATE_MIN_LEN <= (int)sizeof(struct sl2_state_pkt), state_minlen);
SL2_STATIC_ASSERT(SL2_CMD_MIN_LEN   <= (int)sizeof(struct sl2_cmd_pkt),   cmd_minlen);
SL2_STATIC_ASSERT(SL2_PROBE_MIN_LEN <= (int)sizeof(struct sl2_probe_pkt), probe_minlen);
SL2_STATIC_ASSERT(SL2_CAPS_MIN_LEN  <= (int)sizeof(struct sl2_caps_pkt),  caps_minlen);
SL2_STATIC_ASSERT(SL2_PAIR_MIN_LEN  <= (int)sizeof(struct sl2_pair_req_pkt), pair_minlen);
SL2_STATIC_ASSERT(sizeof(struct sl2_dial_info_pkt) + 2 + 112 <= 250,
                  dial_info_cert_fits);   /* ESP-NOW payload ceiling */

/* ── tolerant decode ──────────────────────────────────────────────────── */

/* Zero-fill dst, then copy min(len, dstsz). A shorter floor-era packet leaves
 * its missing tail zeroed; a longer future packet has its unknown tail
 * ignored. Callers gate on `len >= SL2_*_MIN_LEN` first. */
static inline void sl2_decode_pkt(void *dst, size_t dstsz,
                                  const void *src, int len) {
    memset(dst, 0, dstsz);
    size_t n = (size_t)len < dstsz ? (size_t)len : dstsz;
    memcpy(dst, src, n);
}

/* ── temperature display tables ───────────────────────────────────────── */

static inline int16_t sl2_c_to_dc(float c)    { return (int16_t)lroundf(c * 10.0f); }
static inline float   sl2_dc_to_c(int16_t dc) { return (float)dc / 10.0f; }

/* ftab_id 1: Mitsubishi °F table (deliberately non-linear; e.g. 71F=22.0C,
 * 72F=22.5C; 19.5C/20.5C have no °F). Byte-identical to v1 espnow_ftab and
 * the controller web UI's F_TABLE. °F range 61..88. */
#define SL2_FTAB1_MIN_F 61
#define SL2_FTAB1_MAX_F 88
static inline const float *sl2_ftab1(void) {
    static const float tab[SL2_FTAB1_MAX_F - SL2_FTAB1_MIN_F + 1] = {
        16.0f,16.5f,17.0f,17.5f,18.0f,18.5f,19.0f,20.0f,21.0f,21.5f,
        22.0f,22.5f,23.0f,23.5f,24.0f,24.5f,25.0f,25.5f,26.0f,26.5f,
        27.0f,27.5f,28.0f,28.5f,29.0f,29.5f,30.0f,30.5f,
    };
    return tab;
}
static inline float sl2_ftab1_f_to_c(int f) {
    if (f < SL2_FTAB1_MIN_F) f = SL2_FTAB1_MIN_F;
    if (f > SL2_FTAB1_MAX_F) f = SL2_FTAB1_MAX_F;
    return sl2_ftab1()[f - SL2_FTAB1_MIN_F];
}
static inline int sl2_ftab1_c_to_f(float c) {
    const float *tab = sl2_ftab1();
    int bestF = SL2_FTAB1_MIN_F;
    float bestD = 999.0f;
    for (int i = 0; i <= SL2_FTAB1_MAX_F - SL2_FTAB1_MIN_F; i++) {
        float d = fabsf(tab[i] - c);
        if (d < bestD) { bestD = d; bestF = SL2_FTAB1_MIN_F + i; }
    }
    return bestF;
}

/* Wire deci-C -> display integer in the user's unit, per the controller's
 * declared table. Measured temps outside a table's range fall back linear. */
static inline int sl2_dc_to_display(int16_t dc, bool use_f, uint8_t ftab_id) {
    float c = sl2_dc_to_c(dc);
    if (!use_f) return (int)lroundf(c);
    if (ftab_id == 1 && c >= 16.0f && c <= 30.5f) return sl2_ftab1_c_to_f(c);
    return (int)lroundf(c * 9.0f / 5.0f + 32.0f);
}

/* Display integer -> wire deci-C. °C snaps to the 0.5C grid; °F goes through
 * the declared table (linear for ftab_id 0). */
static inline int16_t sl2_display_to_dc(int v, bool use_f, uint8_t ftab_id) {
    if (!use_f) return (int16_t)((int)lroundf((float)v * 2.0f) * 5);
    if (ftab_id == 1) return sl2_c_to_dc(sl2_ftab1_f_to_c(v));
    return sl2_c_to_dc(((float)v - 32.0f) * 5.0f / 9.0f);
}

/* ── misc helpers ─────────────────────────────────────────────────────── */

static inline bool sl2_mac_eq(const uint8_t a[6], const uint8_t b[6]) {
    return memcmp(a, b, 6) == 0;
}

#ifdef __cplusplus
}
#endif
