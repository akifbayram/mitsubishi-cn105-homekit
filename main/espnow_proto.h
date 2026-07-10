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

#define ESPNOW_PROTO_VERSION 11

/* Oldest peer protocol version this firmware still parses. Bump ONLY on a
 * breaking layout change (a field's type/offset moves, or a struct shrinks).
 * ADDITIVE changes — a new flag/mask bit in a byte that already has room, a
 * byte claimed from a `reserved[]` tail, or a field APPENDED to the end of a
 * struct — bump ESPNOW_PROTO_VERSION but leave this floor where it is, so a
 * peer one (or more) versions behind keeps working instead of going silently
 * dark. The receive path accepts any version >= this floor, length-checks
 * against the floor-era ESPNOW_*_MIN_LEN (not sizeof), and decodes with
 * espnow_decode_pkt() (zero-fill + prefix copy) so both shorter old packets and
 * longer future packets parse. The unit and the Dial update independently
 * (unit via OTA, Dial via USB), so this floor is what keeps a mixed-version
 * pair alive between updates.
 *
 * Growth discipline, in order of preference:
 *  1. Claim a spare bit in an existing flags/mask byte.
 *  2. Claim a byte from the struct's trailing `reserved[]` (senders already
 *     zero-fill, receivers already ignore — sizeof does not change).
 *  3. Append a new field after `reserved[]` (sizeof grows; receivers keep
 *     accepting the shorter old packets because the MIN_LEN check is pinned
 *     to the floor-era size).
 * Never insert or resize an existing field — that is the breaking case that
 * bumps this floor and forces a coordinated unit+Dial reflash.
 *
 * v6 (this floor is 5): reserved[] tails added to STATE/CMD/INFO/PROBE. v5
 * peers' shorter packets still parse per rule 3; v5 RECEIVERS, however,
 * version-gate strictly and drop v6 frames, so a v5 Dial shows NOLINK against
 * a v6 unit until reflashed (the unit logs which side is behind).
 * v7: WIFI_REQ/WIFI_RESP (Link OTA credential relay) — new packet types only;
 * old peers ignore unknown types, so the floor is unchanged.
 * v8: STATE gains flags2 (claims reserved[0]) with SF2_SENSOR_BATT_LOW; INFO
 * gains sensor_batt_pct (claims reserved[0]) gated by IF_SENSORBATT_VALID.
 * Additive per rule 2 (reserved byte) — sizeof unchanged, floor stays 5.
 * v9: DIAG packet (unit -> dial device-info page) — new packet type only,
 * pull-gated by PROBE.want_info like INFO; old peers ignore unknown types, so
 * the floor is unchanged.
 * v10: dial-driven Wi-Fi provisioning + HomeKit QR. Four new packet types
 * (WIFI_SCAN_REQ/WIFI_SCAN_RESP/WIFI_SET/WIFI_STATUS — old peers ignore
 * unknown types); STATE flags2 gains SF2_WIFI_PROVISIONED (spare bit, rule 1);
 * INFO appends hk_payload[24] after reserved[] (rule 3 — sizeof grows
 * 77 -> 101, INFO_MIN_LEN stays 75). Floor unchanged.
 * v11: multi-zone dial. PROBE claims reserved[0] as want_ident (rule 2 —
 * sizeof unchanged; this byte was earmarked for a capability/identity claim).
 * New IDENT packet (unit -> dial, unicast/LMK-encrypted, pull-gated by
 * PROBE.want_ident) carries the unit's user-set deviceName for the dial's
 * zone picker. Additive; floor unchanged. */
#define ESPNOW_PROTO_MIN_COMPAT 5

/* Floor-era (v5) wire sizes — the receive path's length checks are pinned to
 * these, NOT to sizeof(), so packets from a MIN_COMPAT-era peer still pass.
 * Only raise these when ESPNOW_PROTO_MIN_COMPAT itself is raised. */
#define ESPNOW_STATE_MIN_LEN 12
#define ESPNOW_INFO_MIN_LEN  75
#define ESPNOW_CMD_MIN_LEN   11
#define ESPNOW_PROBE_MIN_LEN 3
/* v7 packets: new at v7, so their MIN_LEN == their v7 sizeof. */
#define ESPNOW_WIFI_REQ_MIN_LEN  4
#define ESPNOW_WIFI_RESP_MIN_LEN 103

/* v9 packet: new at v9, so its MIN_LEN == its v9 sizeof. */
#define ESPNOW_DIAG_MIN_LEN 56

/* v10 packets: new at v10, so their MIN_LEN == their v10 wire minimum.
 * SCAN_RESP is wire-truncated per page (header + n_items*35), so its check
 * is a header gate + a computed per-item length check in the receiver. */
#define ESPNOW_WIFI_SCAN_REQ_MIN_LEN  4
#define ESPNOW_WIFI_SCAN_RESP_HDR_LEN 6
#define ESPNOW_WIFI_SET_MIN_LEN       102
#define ESPNOW_WIFI_STATUS_MIN_LEN    22

/* v11 packet: new at v11, so its MIN_LEN == its v11 sizeof. */
#define ESPNOW_IDENT_MIN_LEN 36

enum espnow_pkt_type {
    ESPNOW_PKT_STATE = 1,
    ESPNOW_PKT_CMD   = 2,
    ESPNOW_PKT_PROBE = 3,
    ESPNOW_PKT_PAIR_REQ  = 4,   /* dial -> broadcast */
    ESPNOW_PKT_PAIR_RESP = 5,   /* unit -> broadcast, only while window open */
    ESPNOW_PKT_INFO  = 6,       /* unit -> dial, pull-only (PROBE want_info) */
    ESPNOW_PKT_WIFI_REQ  = 7,   /* link -> unit: request home Wi-Fi creds (bonded/encrypted, Link OTA) */
    ESPNOW_PKT_WIFI_RESP = 8,   /* unit -> link: creds reply (bonded/encrypted; ok=0 -> none stored) */
    ESPNOW_PKT_DIAG  = 9,       /* unit -> dial, pull-only (PROBE want_info): device-info page */
    ESPNOW_PKT_WIFI_SCAN_REQ  = 10, /* dial -> unit: scan for networks (bonded/encrypted) */
    ESPNOW_PKT_WIFI_SCAN_RESP = 11, /* unit -> dial: paged scan results (bonded/encrypted) */
    ESPNOW_PKT_WIFI_SET       = 12, /* dial -> unit: join this ssid/psk (bonded/encrypted) */
    ESPNOW_PKT_WIFI_STATUS    = 13, /* unit -> dial: join progress/result (bonded/encrypted) */
    ESPNOW_PKT_IDENT          = 14, /* unit -> dial: deviceName (bonded/encrypted, pull-gated by PROBE.want_ident) */
};

/* STATE flag bits — home-dial status only */
enum {
    ESPNOW_SF_POWER     = 1u << 0,
    ESPNOW_SF_CN105     = 1u << 1,
    ESPNOW_SF_OPERATING = 1u << 2,
    ESPNOW_SF_WIFI      = 1u << 3,
    ESPNOW_SF_HK        = 1u << 4,
    ESPNOW_SF_USE_F     = 1u << 5,
    ESPNOW_SF_VANECFG_LO = 1u << 6,   /* vane_config bit0 (0=none,1=vert,2=vert+horiz) */
    ESPNOW_SF_VANECFG_HI = 1u << 7,   /* vane_config bit1                              */
};

/* vane_config (0=none,1=vertical,2=vertical+horizontal) packed in STATE flags bits 6,7 */
static inline uint8_t espnow_state_vanecfg(uint8_t flags) { return (flags >> 6) & 0x3; }

/* STATE flags2 bits (claims reserved[0]; `flags` bits 0-7 are all assigned) */
enum {
    ESPNOW_SF2_SENSOR_BATT_LOW  = 1u << 0,  /* remote sensor battery critical (unit-thresholded) */
    ESPNOW_SF2_WIFI_PROVISIONED = 1u << 1,  /* v10: unit has stored Wi-Fi creds in NVS. Receivers
                                             * MUST also gate on peer version >= 10 — a v9 unit
                                             * with creds sends this bit as 0 (zero-fill). */
};

/* INFO flag bits — validity travels with the data in espnow_info_pkt */
enum {
    ESPNOW_IF_OUT_VALID     = 1u << 0,
    /* bit 1 reserved (was RUNTIME_VALID; runtime_h dropped in proto v3) */
    ESPNOW_IF_SENSORBATT_VALID = 1u << 2,   /* sensor_batt_pct is valid (v8) */
};

/* DIAG flag bits — validity travels with the data in espnow_diag_pkt */
enum {
    ESPNOW_DF_RUNTIME_VALID   = 1u << 0,   /* runtime_h is valid */
    ESPNOW_DF_BLE_VALID       = 1u << 1,   /* ble_temp_dc/ble_hum_pct/ble_rssi are valid */
    ESPNOW_DF_TEMP_SRC_REMOTE = 1u << 2,   /* unit controls off the BLE remote sensor */
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
    /* Senders zero-fill (memset in buildState()), receivers ignore. `flags` is
     * FULL (bits 0-7 all assigned): the next status bit claims reserved[0] as a
     * `flags2` — VERSION bump, no MIN_COMPAT bump, sizeof unchanged. */
    uint8_t  flags2;        /* v8: ESPNOW_SF2_* (was reserved[0]) */
    uint8_t  reserved[1];   /* v6 */
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
    /* Senders zero-fill (memset in buildInfo()), receivers ignore. iflags also
     * has 6 spare bits — prefer those for new validity flags. */
    uint8_t  sensor_batt_pct; /* v8: remote sensor battery 0-100 (valid per IF_SENSORBATT_VALID; was reserved[0]) */
    uint8_t  reserved[1];   /* v6 */
    /* v10 (rule 3 append): HomeKit X-HM:// setup URI for the dial's QR
     * ("X-HM://XXXXXXXXXYYYY\0", ~21 chars). Empty = unknown/older unit. */
    uint8_t  hk_payload[24];
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
    /* Senders zero-fill (memset before building), receivers ignore. `mask` has
     * one free bit (0x80); its data field claims reserved[0]. */
    uint8_t reserved[1];    /* v6 */
};

struct __attribute__((packed)) espnow_probe_pkt {
    uint8_t type;           /* ESPNOW_PKT_PROBE */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t want_info;      /* 1 while dial is on SYSTEM/HOMEKIT/WIFI screen */
    uint8_t want_ident;     /* v11: 1 -> unit replies IDENT (was reserved[0];
                             * v<=10 senders zero-fill, so it decodes as 0) */
};

/* ── DIAG (v9): unit diagnostics for the dial's About/device-info page ──────
 * Pull-only like INFO (sent while PROBE.want_info is set). Everything here is
 * informational — the dial renders it verbatim and shows "—" when absent. */
struct __attribute__((packed)) espnow_diag_pkt {
    uint8_t  type;            /* ESPNOW_PKT_DIAG */
    uint8_t  version;         /* ESPNOW_PROTO_VERSION */
    uint8_t  dflags;          /* ESPNOW_DF_* */
    uint8_t  fw_ver[24];      /* unit firmware version, null-terminated */
    uint8_t  build_date[12];  /* esp_app_desc date, e.g. "Jul  5 2026" */
    uint32_t uptime_s;        /* unit uptime, seconds */
    uint8_t  reset_reason;    /* raw esp_reset_reason_t value */
    uint8_t  wifi_channel;    /* unit STA channel; 0 = not connected */
    uint8_t  auto_sub_mode;   /* 0=OFF 1=COOL 2=HEAT 3=LEADER (Auto only) */
    uint32_t runtime_h;       /* heat-pump accumulated runtime, hours (DF_RUNTIME_VALID) */
    int16_t  ble_temp_dc;     /* BLE sensor temp, deci-C (DF_BLE_VALID) */
    uint8_t  ble_hum_pct;     /* BLE sensor humidity %; 0xFF = unsupported */
    int8_t   ble_rssi;        /* BLE advertisement RSSI, dBm */
    uint8_t  reserved[2];     /* senders zero-fill, receivers ignore */
};

/* ── IDENT (v11): the unit's user-set name for the dial's zone picker ──────
 * Unicast between bonded peers (LMK-encrypted), pull-gated by
 * PROBE.want_ident and throttled like INFO. The dial persists it in the
 * bond table; an empty name means the unit has none set. */
struct __attribute__((packed)) espnow_ident_pkt {
    uint8_t type;           /* ESPNOW_PKT_IDENT */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t name[32];       /* deviceName, null-terminated */
    uint8_t reserved[2];    /* senders zero-fill, receivers ignore */
};

/* ── Link OTA credential relay (v7) ─────────────────────────────────────────
 * Both packets travel ONLY unicast between bonded peers, so the ESP-NOW LMK
 * encrypts them on the air. The link keeps the credentials in RAM for the
 * duration of one update attempt and zeroizes them afterwards. */
struct __attribute__((packed)) espnow_wifi_req_pkt {
    uint8_t type;           /* ESPNOW_PKT_WIFI_REQ */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t reserved[2];    /* senders zero-fill, receivers ignore */
};

struct __attribute__((packed)) espnow_wifi_resp_pkt {
    uint8_t type;           /* ESPNOW_PKT_WIFI_RESP */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t ok;             /* 1 = ssid/psk valid; 0 = unit has no stored creds */
    uint8_t ssid[33];       /* null-terminated */
    uint8_t psk[65];        /* null-terminated (WPA passphrase max 64) */
    uint8_t reserved[2];    /* senders zero-fill, receivers ignore */
};

/* ── Dial-driven Wi-Fi provisioning (v10) ───────────────────────────────────
 * All four packets travel ONLY unicast between bonded peers (LMK-encrypted).
 * The unit scans/joins — its antenna is the one that matters — and the dial
 * is pure UI. After a WIFI_SET the unit pushes WIFI_STATUS ~1 Hz for 60 s so
 * the dial hears the outcome even across the join's channel change (the
 * dial's sweep re-locks onto the new channel within a few seconds). */
struct __attribute__((packed)) espnow_wifi_scan_req_pkt {
    uint8_t type;           /* ESPNOW_PKT_WIFI_SCAN_REQ */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t reserved[2];    /* senders zero-fill, receivers ignore */
};

#define ESPNOW_WIFI_SCAN_MAX_ITEMS 6   /* per page: 6 + 6*35 = 216 B < 250 B */
struct __attribute__((packed)) espnow_wifi_scan_item {
    uint8_t ssid[33];       /* null-terminated */
    int8_t  rssi;
    uint8_t secure;         /* 1 = any auth mode, 0 = open */
};

struct __attribute__((packed)) espnow_wifi_scan_resp_pkt {
    uint8_t type;           /* ESPNOW_PKT_WIFI_SCAN_RESP */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t page;           /* 0-based */
    uint8_t n_pages;        /* total pages in this scan (>= 1; 1 with n_items 0 = none found) */
    uint8_t n_items;        /* items valid in THIS page */
    uint8_t reserved[1];    /* senders zero-fill, receivers ignore */
    struct espnow_wifi_scan_item items[ESPNOW_WIFI_SCAN_MAX_ITEMS];
    /* Wire frames are truncated to HDR_LEN + n_items*35; receivers gate on
     * len >= HDR_LEN, then on len >= HDR_LEN + n_items*35. */
};

struct __attribute__((packed)) espnow_wifi_set_pkt {
    uint8_t type;           /* ESPNOW_PKT_WIFI_SET */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t ssid[33];       /* null-terminated */
    uint8_t psk[65];        /* null-terminated (empty = open network) */
    uint8_t reserved[2];    /* senders zero-fill, receivers ignore */
};

enum espnow_wifi_status {
    ESPNOW_WIFI_ST_CONNECTING   = 1,
    ESPNOW_WIFI_ST_CONNECTED    = 2,
    ESPNOW_WIFI_ST_AUTH_FAIL    = 3,   /* wrong password (auth-class disconnect) */
    ESPNOW_WIFI_ST_AP_NOT_FOUND = 4,
    ESPNOW_WIFI_ST_TIMEOUT      = 5,   /* no success within the unit's 30 s window */
};

struct __attribute__((packed)) espnow_wifi_status_pkt {
    uint8_t type;           /* ESPNOW_PKT_WIFI_STATUS */
    uint8_t version;        /* ESPNOW_PROTO_VERSION */
    uint8_t status;         /* enum espnow_wifi_status */
    int8_t  rssi;           /* unit STA RSSI (valid when CONNECTED) */
    uint8_t ip[16];         /* "255.255.255.255\0" (valid when CONNECTED) */
    uint8_t reserved[2];    /* senders zero-fill, receivers ignore */
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
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_state_pkt) == 14, state_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_info_pkt)  == 101, info_size);   /* v10: +hk_payload[24] */
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_cmd_pkt)   == 12, cmd_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_probe_pkt) == 4,  probe_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_wifi_req_pkt)  == 4,   wifi_req_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_wifi_resp_pkt) == 103, wifi_resp_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_wifi_scan_req_pkt) == 4, wifi_scan_req_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_wifi_scan_item) == 35, wifi_scan_item_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_wifi_scan_resp_pkt) == 216, wifi_scan_resp_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_wifi_set_pkt) == 102, wifi_set_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_wifi_status_pkt) == 22, wifi_status_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_pair_req_pkt)  == 56, pair_req_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_pair_resp_pkt) == 56, pair_resp_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_diag_pkt) == 56, diag_size);
ESPNOW_STATIC_ASSERT(sizeof(struct espnow_ident_pkt) == 36, ident_size);
ESPNOW_STATIC_ASSERT(ESPNOW_IDENT_MIN_LEN <= (int)sizeof(struct espnow_ident_pkt), ident_minlen);
/* MIN_LEN never exceeds the current wire size. */
ESPNOW_STATIC_ASSERT(ESPNOW_STATE_MIN_LEN <= (int)sizeof(struct espnow_state_pkt), state_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_INFO_MIN_LEN  <= (int)sizeof(struct espnow_info_pkt),  info_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_CMD_MIN_LEN   <= (int)sizeof(struct espnow_cmd_pkt),   cmd_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_PROBE_MIN_LEN <= (int)sizeof(struct espnow_probe_pkt), probe_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_WIFI_REQ_MIN_LEN  <= (int)sizeof(struct espnow_wifi_req_pkt),  wifi_req_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_WIFI_RESP_MIN_LEN <= (int)sizeof(struct espnow_wifi_resp_pkt), wifi_resp_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_WIFI_SCAN_REQ_MIN_LEN <= (int)sizeof(struct espnow_wifi_scan_req_pkt), wifi_scan_req_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_WIFI_SET_MIN_LEN <= (int)sizeof(struct espnow_wifi_set_pkt), wifi_set_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_WIFI_STATUS_MIN_LEN <= (int)sizeof(struct espnow_wifi_status_pkt), wifi_status_minlen);
ESPNOW_STATIC_ASSERT(ESPNOW_DIAG_MIN_LEN <= (int)sizeof(struct espnow_diag_pkt), diag_minlen);

/* ── pure helpers ─────────────────────────────────────────────────────── */

/* Tolerant packet decode: zero-fill the struct, then copy min(len, dstsz).
 * A shorter MIN_COMPAT-era packet leaves its missing tail (reserved bytes /
 * appended fields) zeroed; a longer future packet has its unknown tail
 * ignored. Callers gate on `len >= ESPNOW_*_MIN_LEN` first. */
static inline void espnow_decode_pkt(void *dst, size_t dstsz,
                                     const void *src, int len) {
    memset(dst, 0, dstsz);
    size_t n = (size_t)len < dstsz ? (size_t)len : dstsz;
    memcpy(dst, src, n);
}

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
                                              bool wifi, bool hk, bool use_f,
                                              uint8_t vane_config) {
    uint8_t f = 0;
    if (power)     f |= ESPNOW_SF_POWER;
    if (cn105)     f |= ESPNOW_SF_CN105;
    if (operating) f |= ESPNOW_SF_OPERATING;
    if (wifi)      f |= ESPNOW_SF_WIFI;
    if (hk)        f |= ESPNOW_SF_HK;
    if (use_f)     f |= ESPNOW_SF_USE_F;
    f |= (uint8_t)((vane_config & 0x3) << 6);
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
