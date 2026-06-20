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

#define ESPNOW_PROTO_VERSION 1

enum espnow_pkt_type {
    ESPNOW_PKT_STATE = 1,
    ESPNOW_PKT_CMD   = 2,
    ESPNOW_PKT_PROBE = 3,
    ESPNOW_PKT_PAIR_REQ  = 4,   /* dial -> broadcast */
    ESPNOW_PKT_PAIR_RESP = 5,   /* unit -> broadcast, only while window open */
};

/* STATE flag bits */
enum {
    ESPNOW_SF_POWER         = 1u << 0,
    ESPNOW_SF_CN105         = 1u << 1,
    ESPNOW_SF_OPERATING     = 1u << 2,
    ESPNOW_SF_WIFI          = 1u << 3,
    ESPNOW_SF_HK            = 1u << 4,
    ESPNOW_SF_USE_F         = 1u << 5,
    ESPNOW_SF_OUT_VALID     = 1u << 6,
    ESPNOW_SF_RUNTIME_VALID = 1u << 7,
};

/* CMD field-mask bits */
enum {
    ESPNOW_CM_POWER = 1u << 0,
    ESPNOW_CM_MODE  = 1u << 1,
    ESPNOW_CM_TEMP  = 1u << 2,
    ESPNOW_CM_FAN   = 1u << 3,
};

struct __attribute__((packed)) espnow_state_pkt {
    uint8_t  type;          /* ESPNOW_PKT_STATE */
    uint8_t  version;       /* ESPNOW_PROTO_VERSION */
    uint8_t  flags;         /* ESPNOW_SF_* */
    uint8_t  mode;          /* CN105 mode byte */
    uint8_t  fan;           /* CN105 fan byte */
    uint8_t  vane;          /* CN105 vane byte */
    uint8_t  wide_vane;     /* CN105 wide vane byte */
    uint8_t  compressor_hz;  /* raw CN105 byte; 0-255 Hz */
    uint8_t  sub_mode;
    uint8_t  stage;
    uint8_t  auto_sub_mode;
    uint8_t  error_code;    /* 0x80 = normal */
    int16_t  room_dc;       /* room temp, deci-C */
    int16_t  set_dc;        /* setpoint, deci-C */
    int16_t  outside_dc;    /* outside temp, deci-C */
    uint16_t runtime_h;     /* runtime hours (whole) */
    uint8_t  hk_paired;     /* paired controller count */
    int8_t   wifi_rssi;
    uint8_t  ssid[33];      /* null-terminated */
    uint8_t  ip[16];        /* "255.255.255.255\0" */
};

struct __attribute__((packed)) espnow_cmd_pkt {
    uint8_t type;           /* ESPNOW_PKT_CMD */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t mask;           /* ESPNOW_CM_* */
    uint8_t power;          /* 0/1 */
    uint8_t mode;           /* CN105 mode byte */
    uint8_t fan;            /* CN105 fan byte */
    int16_t set_dc;         /* setpoint, deci-C */
};

struct __attribute__((packed)) espnow_probe_pkt {
    uint8_t type;           /* ESPNOW_PKT_PROBE */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
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
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_state_pkt) == 71, state_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_cmd_pkt)   == 8,  cmd_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_probe_pkt) == 2,  probe_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_pair_req_pkt)  == 56, pair_req_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_pair_resp_pkt) == 56, pair_resp_size);

/* ── pure helpers ─────────────────────────────────────────────────────── */

static inline int16_t espnow_c_to_dc(float c)   { return (int16_t)lroundf(c * 10.0f); }
static inline float   espnow_dc_to_c(int16_t dc) { return (float)dc / 10.0f; }

/* Convert wire deci-C to an integer for display in the user's unit. */
static inline int espnow_dc_to_display(int16_t dc, bool use_f) {
    float c = (float)dc / 10.0f;
    return use_f ? (int)lroundf(c * 9.0f / 5.0f + 32.0f) : (int)lroundf(c);
}

/* Convert a display-unit integer back to deci-C, snapped to the 0.5 C grid. */
static inline int16_t espnow_display_to_dc(int v, bool use_f) {
    float c = use_f ? (((float)v - 32.0f) * 5.0f / 9.0f) : (float)v;
    return (int16_t)((int)lroundf(c * 2.0f) * 5); /* 0.5C steps -> tenths */
}

static inline uint8_t espnow_make_state_flags(bool power, bool cn105, bool operating,
                                              bool wifi, bool hk, bool use_f,
                                              bool out_valid, bool runtime_valid) {
    uint8_t f = 0;
    if (power)         f |= ESPNOW_SF_POWER;
    if (cn105)         f |= ESPNOW_SF_CN105;
    if (operating)     f |= ESPNOW_SF_OPERATING;
    if (wifi)          f |= ESPNOW_SF_WIFI;
    if (hk)            f |= ESPNOW_SF_HK;
    if (use_f)         f |= ESPNOW_SF_USE_F;
    if (out_valid)     f |= ESPNOW_SF_OUT_VALID;
    if (runtime_valid) f |= ESPNOW_SF_RUNTIME_VALID;
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
