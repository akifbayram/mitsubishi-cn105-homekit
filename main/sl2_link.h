/*
 * sl2_link.h — Serin Link controller-role core.
 *
 * Owns: signed pairing + TOFU pinning, multi-dial bond table, LMK derivation,
 * STATE fan-out (change/heartbeat), CAPS/INFO pull gates, CMD apply + echo to
 * all dials, WIFI_REQ->RESP, per-dial liveness. Platform enters through
 * sl2_port_t / sl2_crypto_t; the HVAC device through sl2_hvac_iface_t.
 *
 * Threading contract: sl2_link_on_recv() and sl2_link_loop() must run in the
 * same context. Queue radio callbacks through sl2_rxq.h if they don't.
 */
#pragma once
#include "sl2_proto.h"
#include "sl2_port.h"
#include "sl2_crypto.h"
#include "sl2_bond.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Semantic device state the adapter reports; the core turns it into STATE. */
typedef struct {
    bool     hvac_link;       /* controller <-> heat pump serial link up */
    bool     wifi;            /* controller STA associated */
    bool     wifi_provisioned;/* has stored STA creds */
    bool     setup_ap;        /* recovery/setup hotspot active (SL2_SF_SETUP_AP) */
    bool     wifi_err;        /* last credential change failed (SL2_SF_WIFI_ERR) */
    bool     use_f;           /* display pref */
    bool     sensor_batt_low;
    uint8_t  mode;            /* enum sl2_mode */
    uint8_t  action;          /* enum sl2_action */
    uint8_t  fan;             /* 0=auto, 1..100 */
    uint8_t  vane_v, vane_h;  /* 0=auto, 1..n, 255=swing */
    uint8_t  preset;          /* enum sl2_preset */
    uint16_t fault;           /* 0 = normal */
    int16_t  room_dc;
    int16_t  set_dc;
    int16_t  set_low_dc;      /* SL2_DC_NA when n/a */
    int16_t  set_high_dc;     /* SL2_DC_NA when n/a */
    uint8_t  room_hum_pct;    /* SL2_HUM_NA when not reported */
    uint8_t  hum_set_pct;     /* SL2_HUM_NA when n/a */
} sl2_hvac_state_t;

/* hvac_link policy for adapters whose platform lacks a native link-health
 * signal (e.g. a generic ESPHome climate entity, which exists whether or not
 * the device behind it answers). An explicit source (driver flag, YAML
 * lambda) always wins. Otherwise: an entity that claims a room-temperature
 * reading but reports none (NaN) has never heard from the device — link
 * down. Entities with no room-temp feedback channel at all (IR blasters)
 * have nothing to monitor: report the link up rather than crying wolf. */
static inline bool sl2_hvac_link_infer(bool has_health, bool health,
                                       bool reports_room_temp, float room_c) {
    if (has_health) return health;
    return !(reports_room_temp && isnan(room_c));
}

typedef struct sl2_hvac_iface {
    void *ctx;
    bool (*get_state)(void *ctx, sl2_hvac_state_t *out);
    /* Apply masked fields. The core echoes STATE to all dials afterwards. */
    bool (*apply)(void *ctx, uint16_t mask, const struct sl2_cmd_pkt *cmd);
    /* Fill everything except type/version/caps_seq (core stamps those). */
    bool (*get_caps)(void *ctx, struct sl2_caps_pkt *out);
    /* Append TLVs for INFO; return bytes written (0 ok). */
    size_t (*fill_info_tlvs)(void *ctx, uint8_t *buf, size_t cap);
    /* Link OTA: STA creds. Return false (or NULL hook) when unavailable —
     * the core then answers ok=0 and CAPS should omit SL2_FEAT_LINK_OTA_CREDS. */
    bool (*wifi_creds)(void *ctx, char ssid[33], char psk[65]);
    /* Dial asked for the setup hotspot (WIFI_SETUP): raise the recovery AP /
     * change-network window now. Optional: NULL = unsupported — CAPS should
     * then omit SL2_FEAT_WIFI_SETUP. Must be idempotent: dials re-send ~1 Hz
     * until STATE shows SL2_SF_SETUP_AP. */
    bool (*wifi_setup)(void *ctx);
} sl2_hvac_iface_t;

/* Per-dial runtime slot (private). */
typedef struct {
    sl2_dial_bond_t bond;
    uint32_t last_probe_ms;
    uint32_t last_state_tx_ms;
    uint32_t last_caps_tx_ms;
    uint32_t last_info_tx_ms;
    uint8_t  want;            /* latched from the dial's last PROBE */
    bool     was_live;
    bool     pend_state;      /* STATE changed since this dial last got one */
    bool     wifi_req;        /* Link OTA creds request pending */
    bool     wifi_setup_req;  /* setup-AP request pending */
    char     model[24];       /* dial identity (DIAL_INFO); "" until received */
    char     fw[16];
    uint8_t  peer_caps_seq;   /* caps_seq the dial reports applied */
    bool     have_info;       /* a DIAL_INFO has been received */
} sl2_dial_rt_t;

typedef enum { SL2_PAIR_OFF = 0, SL2_PAIR_WINDOW, SL2_PAIR_CONFIRM } sl2_pair_state_t;

/* All fields private; allocate one, zero it via sl2_link_init(). */
typedef struct sl2_link {
    const sl2_port_t *port;
    const sl2_crypto_t *crypto;
    const sl2_hvac_iface_t *hvac;
    uint8_t  own_mac[6];
    /* identity */
    uint8_t  id_priv[SL2_ED25519_PRIV];
    uint8_t  id_pub[SL2_ED25519_PUB];
    /* bonds */
    sl2_dial_rt_t dial[SL2_MAX_DIALS];
    int      n_dials;
    /* shared STATE change detection */
    struct sl2_state_pkt last_state;
    bool     have_last_state;
    /* pairing */
    sl2_pair_state_t pair;
    uint32_t pair_deadline_ms;
    uint32_t confirm_deadline_ms;
    uint8_t  eph_priv[32];    /* X25519 ephemeral, one per pairing window */
    uint8_t  eph_pub[32];
    uint8_t  cand_mac[6];
    uint8_t  cand_lmk[16];
    uint8_t  cand_id_pub[32];
    const char *pair_result;
    uint8_t  caps_seq;
    uint16_t epoch;           /* random nonzero per boot; 0 = rand failed
                               * (replay guard off, fail open) */
    bool     started;
} sl2_link_t;

/* Wire-level cadence (ms). Overridable for tests via -D. */
#ifndef SL2_STATE_MIN_INTERVAL_MS
#define SL2_STATE_MIN_INTERVAL_MS 250
#endif
#ifndef SL2_STATE_HEARTBEAT_MS
#define SL2_STATE_HEARTBEAT_MS    10000
#endif
#ifndef SL2_PULL_THROTTLE_MS
#define SL2_PULL_THROTTLE_MS      2000
#endif
/* Dial-liveness window: STATE flows only to dials that probed within this.
 * Must survive ONE lost probe at the dial's slowest keepalive cadence —
 * the dial fw probes background zones every 4 s + up to 1.8 s of per-zone
 * stagger (BG_PROBE_MS in espnow_client.c), so two worst-case periods are
 * 11.6 s. Cutting the stream on a single lost probe let background zones go
 * stale and turned later zone switches into cold resyncs. Cost of the wider
 * window is only a few extra ~50 B heartbeats after a dial truly vanishes. */
#ifndef SL2_DIAL_LIVE_MS
#define SL2_DIAL_LIVE_MS          12500
#endif
#ifndef SL2_PAIR_CONFIRM_MS
#define SL2_PAIR_CONFIRM_MS       15000
#endif

void sl2_link_init(sl2_link_t *l, const sl2_port_t *port,
                   const sl2_crypto_t *crypto, const sl2_hvac_iface_t *hvac);
/* Load-or-generate identity, load bonds, install encrypted peers.
 * Returns false on port/crypto failure (core stays inert). */
bool sl2_link_start(sl2_link_t *l);
void sl2_link_loop(sl2_link_t *l);
void sl2_link_on_recv(sl2_link_t *l, const uint8_t src[6], const uint8_t dst[6],
                      const uint8_t *data, int len);

/* Pairing window (button/UI-gated by the adapter). */
void sl2_link_pair_start(sl2_link_t *l, uint32_t window_ms);
void sl2_link_pair_cancel(sl2_link_t *l);
bool sl2_link_pairing(const sl2_link_t *l);
int  sl2_link_pair_seconds_left(sl2_link_t *l);
const char *sl2_link_pair_result(const sl2_link_t *l);   /* "idle"/"listening"/"confirming"/"paired"/"timeout"/"full"/"pin-mismatch" */

/* Bond management. */
int  sl2_link_dial_count(const sl2_link_t *l);
bool sl2_link_dial_mac(const sl2_link_t *l, int idx, uint8_t out[6]);
bool sl2_link_forget_dial(sl2_link_t *l, const uint8_t mac[6]);
void sl2_link_forget_all(sl2_link_t *l);

/* Liveness. */
bool sl2_link_dial_live(sl2_link_t *l, int idx);
bool sl2_link_any_live(sl2_link_t *l);

/* Read-only snapshot of one bonded dial for the platform UI. Computed with the
 * port clock, so last_seen_ms is epoch-safe across adapter/core. */
typedef struct {
    uint8_t  mac[6];
    bool     live;
    int32_t  last_seen_ms;    /* now - last heard; -1 if never */
    bool     have_info;
    char     model[24];
    char     fw[16];
    uint8_t  caps_seq;        /* dial's applied caps_seq */
} sl2_dial_view_t;

bool sl2_link_dial_view(sl2_link_t *l, int idx, sl2_dial_view_t *out);
uint8_t sl2_link_caps_seq(const sl2_link_t *l);

/* Call when caps content (incl. zone name) changes: bumps + persists caps_seq
 * so dials re-pull. */
void sl2_link_caps_changed(sl2_link_t *l);

/* KV keys used through the port (documented for adapters/tools). */
#define SL2_KV_IDENTITY "sl2_id"     /* 96 B: ed25519 priv(64) || pub(32) */
#define SL2_KV_BONDS    "sl2_dials"  /* sl2_bond.h blob */
#define SL2_KV_CAPS_SEQ "sl2_cseq"   /* 1 B */

#ifdef __cplusplus
}
#endif
