/*
 * espnow_proto.h — wire format for the Dial<->unit ESP-NOW link.
 *
 * Dependency-free C: shared byte-identical between the firmware (main/) and the
 * Dial firmware (private repo Serin-Labs/serin-dial, main/). This firmware copy
 * is the source of truth; keep the vendored Dial copy in sync (version byte +
 * static_assert guards catch drift). Both ends and the host test are
 * little-endian, so the packed structs ARE the wire format (encode/decode ==
 * memcpy).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_PROTO_VERSION 4

enum espnow_pkt_type {
    ESPNOW_PKT_STATE = 1,
    ESPNOW_PKT_CMD   = 2,
    ESPNOW_PKT_PROBE = 3,
    ESPNOW_PKT_PAIR_REQ  = 4,   /* dial -> broadcast */
    ESPNOW_PKT_PAIR_RESP = 5,   /* unit -> broadcast, only while window open */
    ESPNOW_PKT_INFO  = 6,       /* unit -> dial, pull-only (PROBE want_info) */
};

/* STATE flag bits — home-dial status only */
enum {
    ESPNOW_SF_POWER     = 1u << 0,
    ESPNOW_SF_CN105     = 1u << 1,
    ESPNOW_SF_OPERATING = 1u << 2,
    ESPNOW_SF_WIFI      = 1u << 3,
    ESPNOW_SF_HK        = 1u << 4,
    ESPNOW_SF_USE_F     = 1u << 5,
    /* bits 6,7 reserved (were OUT_VALID/RUNTIME_VALID; now in INFO iflags) */
};

/* INFO flag bits — validity travels with the data in espnow_info_pkt */
enum {
    ESPNOW_IF_OUT_VALID     = 1u << 0,
    /* bit 1 reserved (was RUNTIME_VALID; runtime_h dropped in proto v3) */
};

/* CMD field-mask bits */
enum {
    ESPNOW_CM_POWER    = 1u << 0,
    ESPNOW_CM_MODE     = 1u << 1,
    ESPNOW_CM_TEMP     = 1u << 2,
    ESPNOW_CM_FAN      = 1u << 3,
    ESPNOW_CM_VANE     = 1u << 4,   /* vertical vane (proto v3) */
    ESPNOW_CM_WIDEVANE = 1u << 5,   /* horizontal/wide vane (proto v3) */
    ESPNOW_CM_UNITS    = 1u << 6,   /* °C/°F display unit, unit-wide (proto v4) */
};

struct __attribute__((packed)) espnow_state_pkt {
    uint8_t  type;          /* ESPNOW_PKT_STATE */
    uint8_t  version;       /* ESPNOW_PROTO_VERSION */
    uint8_t  flags;         /* ESPNOW_SF_* */
    uint8_t  mode;          /* CN105 mode byte */
    uint8_t  fan;           /* CN105 fan byte */
    uint8_t  vane;          /* CN105 vane byte */
    uint8_t  wide_vane;     /* CN105 wide vane byte */
    uint8_t  error_code;    /* 0x80 = normal; drives home-dial fault alert */
    int16_t  room_dc;       /* room temp, deci-C */
    int16_t  set_dc;        /* setpoint, deci-C */
};

struct __attribute__((packed)) espnow_info_pkt {
    uint8_t  type;          /* ESPNOW_PKT_INFO */
    uint8_t  version;       /* ESPNOW_PROTO_VERSION */
    uint8_t  iflags;        /* ESPNOW_IF_* */
    uint8_t  compressor_hz;
    uint8_t  sub_mode;      /* 0x00=NORMAL,0x02=DEFROST,0x04=PREHEAT,0x08=STANDBY */
    uint8_t  stage;
    int16_t  outside_dc;    /* outside temp, deci-C (valid per ESPNOW_IF_OUT_VALID) */
    uint8_t  hk_paired;     /* paired controller count */
    int8_t   wifi_rssi;
    uint8_t  ssid[33];      /* null-terminated */
    uint8_t  ip[16];        /* "255.255.255.255\0" */
    uint8_t  hk_code[16];   /* "XXX-XX-XXX\0" */
};

struct __attribute__((packed)) espnow_cmd_pkt {
    uint8_t type;           /* ESPNOW_PKT_CMD */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t mask;           /* ESPNOW_CM_* */
    uint8_t power;          /* 0/1 */
    uint8_t mode;           /* CN105 mode byte */
    uint8_t fan;            /* CN105 fan byte */
    int16_t set_dc;         /* setpoint, deci-C */
    uint8_t vane;           /* CN105 vane byte (ESPNOW_CM_VANE) */
    uint8_t wide_vane;      /* CN105 wide vane byte (ESPNOW_CM_WIDEVANE) */
    uint8_t use_f;          /* 0=°C 1=°F, unit-wide display unit (ESPNOW_CM_UNITS) */
};

struct __attribute__((packed)) espnow_probe_pkt {
    uint8_t type;           /* ESPNOW_PKT_PROBE */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t want_info;      /* 1 while dial is on SYSTEM/HOMEKIT/WIFI screen */
};

struct __attribute__((packed)) espnow_pair_req_pkt {
    uint8_t type;        /* ESPNOW_PKT_PAIR_REQ */
    uint8_t version;     /* ESPNOW_PROTO_VERSION */
    uint8_t src_mac[6];  /* dial STA MAC */
    uint8_t pub[32];     /* dial ephemeral X25519 public key */
    uint8_t tag[16];     /* HMAC-SHA256(PMK, req_transcript)[:16] */
};

struct __attribute__((packed)) espnow_pair_resp_pkt {
    uint8_t type;        /* ESPNOW_PKT_PAIR_RESP */
    uint8_t version;     /* ESPNOW_PROTO_VERSION */
    uint8_t src_mac[6];  /* unit STA MAC */
    uint8_t pub[32];     /* unit ephemeral X25519 public key */
    uint8_t tag[16];     /* HMAC-SHA256(PMK, resp_transcript incl. dial pub)[:16] */
};

/* sizeof guards — both copies of this header must agree */
#define ESPNOW_STATIC_ASSERT(c, m) typedef char espnow_sa_##m[(c) ? 1 : -1]
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_state_pkt) == 12, state_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_info_pkt)  == 75, info_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_cmd_pkt)   == 11, cmd_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_probe_pkt) == 3,  probe_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_pair_req_pkt)  == 56, pair_req_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_pair_resp_pkt) == 56, pair_resp_size);

/* ── pure helpers ─────────────────────────────────────────────────────── */

static inline int16_t espnow_c_to_dc(float c)   { return (int16_t)lroundf(c * 10.0f); }
static inline float   espnow_dc_to_c(int16_t dc) { return (float)dc / 10.0f; }

/* ── Mitsubishi °F<->°C setpoint table ────────────────────────────────────
 * °F display is NOT linear on these units: the table below MUST stay
 * byte-identical to F_TABLE in web/index.html so the Dial, the web UI and the
 * unit all agree on which °C a given °F means (and back). It is deliberately
 * non-linear — e.g. 71°F=22.0C, 72°F=22.5C — and 19.5C/20.5C have no °F
 * representation. Using plain linear math here made the Dial send 22.0C for
 * "72F", which the unit/web read back as 71F. °F range 61..88. */
#define ESPNOW_FTAB_MIN_F 61
#define ESPNOW_FTAB_MAX_F 88
static inline const float *espnow_ftab(void) {
    static const float tab[ESPNOW_FTAB_MAX_F - ESPNOW_FTAB_MIN_F + 1] = {
        16.0f,16.5f,17.0f,17.5f,18.0f,18.5f,19.0f,20.0f,21.0f,21.5f,
        22.0f,22.5f,23.0f,23.5f,24.0f,24.5f,25.0f,25.5f,26.0f,26.5f,
        27.0f,27.5f,28.0f,28.5f,29.0f,29.5f,30.0f,30.5f,
    };
    return tab;
}
/* whole °F -> table °C (clamped to range). Mirrors web fToCTable(). */
static inline float espnow_ftab_f_to_c(int f) {
    if (f < ESPNOW_FTAB_MIN_F) f = ESPNOW_FTAB_MIN_F;
    if (f > ESPNOW_FTAB_MAX_F) f = ESPNOW_FTAB_MAX_F;
    return espnow_ftab()[f - ESPNOW_FTAB_MIN_F];
}
/* °C -> nearest table °F (lowest °F wins on a tie). Mirrors web cToFTable(). */
static inline int espnow_ftab_c_to_f(float c) {
    const float *tab = espnow_ftab();
    int bestF = ESPNOW_FTAB_MIN_F;
    float bestD = 999.0f;
    for (int i = 0; i <= ESPNOW_FTAB_MAX_F - ESPNOW_FTAB_MIN_F; i++) {
        float d = fabsf(tab[i] - c);
        if (d < bestD) { bestD = d; bestF = ESPNOW_FTAB_MIN_F + i; }
    }
    return bestF;
}

/* Convert wire deci-C to an integer for display in the user's unit. */
static inline int espnow_dc_to_display(int16_t dc, bool use_f) {
    float c = (float)dc / 10.0f;
    if (!use_f) return (int)lroundf(c);
    /* Setpoints live within the table's range; measured temps (room/outside)
     * can fall outside it -> linear fallback, matching web fmtReadingTemp(). */
    if (c >= 16.0f && c <= 30.5f) return espnow_ftab_c_to_f(c);
    return (int)lroundf(c * 9.0f / 5.0f + 32.0f);
}

/* Convert a display-unit integer back to deci-C. For °F this uses the
 * Mitsubishi table so a setpoint dialed in °F maps to exactly the °C the web
 * UI/unit mean by that °F; for °C it snaps to the 0.5 C grid. */
static inline int16_t espnow_display_to_dc(int v, bool use_f) {
    if (use_f) return (int16_t)lroundf(espnow_ftab_f_to_c(v) * 10.0f);
    return (int16_t)((int)lroundf((float)v * 2.0f) * 5); /* 0.5C steps -> tenths */
}

static inline uint8_t espnow_make_state_flags(bool power, bool cn105, bool operating,
                                              bool wifi, bool hk, bool use_f) {
    uint8_t f = 0;
    if (power)     f |= ESPNOW_SF_POWER;
    if (cn105)     f |= ESPNOW_SF_CN105;
    if (operating) f |= ESPNOW_SF_OPERATING;
    if (wifi)      f |= ESPNOW_SF_WIFI;
    if (hk)        f |= ESPNOW_SF_HK;
    if (use_f)     f |= ESPNOW_SF_USE_F;
    return f;
}

static inline uint8_t espnow_make_info_flags(bool out_valid) {
    uint8_t f = 0;
    if (out_valid) f |= ESPNOW_IF_OUT_VALID;
    return f;
}

static inline int espnow__hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool espnow_parse_mac(const char *s, uint8_t out[6]) {
    if (!s) return false;
    for (int i = 0; i < 6; i++) {
        int hi = espnow__hexval(s[0]), lo = espnow__hexval(s[1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i < 5) { if (*s != ':') return false; s++; }
    }
    return *s == '\0';
}

static inline bool espnow_parse_hex16(const char *s, uint8_t out[16]) {
    if (!s) return false;
    for (int i = 0; i < 16; i++) {
        int hi = espnow__hexval(s[0]);
        if (hi < 0) return false;
        int lo = espnow__hexval(s[1]);
        if (lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return *s == '\0';
}

/* HMAC transcript for a PAIR_REQ: type|version|src_mac|pub  (40 bytes). */
static inline size_t espnow_pair_req_transcript(const struct espnow_pair_req_pkt *p,
                                                uint8_t out[40]) {
    out[0] = p->type; out[1] = p->version;
    memcpy(out + 2, p->src_mac, 6);
    memcpy(out + 8, p->pub, 32);
    return 40;
}

/* HMAC transcript for a PAIR_RESP: type|version|src_mac|pub|dial_pub  (72 bytes).
 * Binding dial_pub proves the response answers THIS request. */
static inline size_t espnow_pair_resp_transcript(const struct espnow_pair_resp_pkt *p,
                                                  const uint8_t peer_pub[32],
                                                  uint8_t out[72]) {
    out[0] = p->type; out[1] = p->version;
    memcpy(out + 2, p->src_mac, 6);
    memcpy(out + 8, p->pub, 32);
    memcpy(out + 40, peer_pub, 32);
    return 72;
}

/* HKDF salt = dial_pub || unit_pub (64 bytes), same order on both ends. */
static inline void espnow_pair_salt(const uint8_t dial_pub[32],
                                    const uint8_t unit_pub[32], uint8_t out[64]) {
    memcpy(out, dial_pub, 32);
    memcpy(out + 32, unit_pub, 32);
}

#ifdef __cplusplus
}
#endif
