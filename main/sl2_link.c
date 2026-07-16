/*
 * sl2_link.c — Serin Link controller-role core. Platform-free; see
 * sl2_port.h / sl2_crypto.h / sl2_link.h for the contracts.
 */
#include "sl2_link.h"

/* ── small helpers ────────────────────────────────────────────────────── */

static void lg(const sl2_link_t *l, int lvl, const char *msg) {
    if (l->port->log) l->port->log(l->port->ctx, lvl, msg);
}

static sl2_dial_rt_t *dial_by_mac(sl2_link_t *l, const uint8_t mac[6]) {
    for (int i = 0; i < l->n_dials; i++)
        if (sl2_mac_eq(l->dial[i].bond.mac, mac)) return &l->dial[i];
    return NULL;
}

static bool dial_live_at(const sl2_dial_rt_t *d, uint32_t now) {
    return d->last_probe_ms != 0 &&
           (uint32_t)(now - d->last_probe_ms) < SL2_DIAL_LIVE_MS;
}

static bool persist_bonds(sl2_link_t *l) {
    sl2_dial_bond_t recs[SL2_MAX_DIALS];
    for (int i = 0; i < l->n_dials; i++) recs[i] = l->dial[i].bond;
    uint8_t blob[SL2_BONDS_BLOB_MAX];
    size_t len = sl2_bonds_encode(recs, l->n_dials, blob, sizeof blob);
    if (!len) return false;
    return l->port->kv_set(l->port->ctx, SL2_KV_BONDS, blob, len);
}

static void persist_caps_seq(sl2_link_t *l) {
    l->port->kv_set(l->port->ctx, SL2_KV_CAPS_SEQ, &l->caps_seq, 1);
}

static void mark_all_state_pending(sl2_link_t *l) {
    for (int i = 0; i < l->n_dials; i++) l->dial[i].pend_state = true;
}

/* Replay guard: dials echo STATE.epoch in CMD/WIFI_SETUP. The radio's CCMP
 * packet-number window lives in RAM, so a captured ciphertext replayed after
 * a controller reboot decrypts cleanly — the per-boot epoch is what kills it
 * here. Enforcement ratchets on per dial (persisted bond flag) the first time
 * a correct echo arrives, so pre-epoch dial firmware keeps working until it
 * upgrades. Returns true if the packet may be acted on. */
static bool epoch_ok(sl2_link_t *l, sl2_dial_rt_t *d, uint16_t e) {
    if (l->epoch == 0) return true;        /* rand failed at boot: guard off */
    if (e == l->epoch) {
        if (!(d->bond.flags & SL2_BOND_F_EPOCH)) {
            d->bond.flags |= SL2_BOND_F_EPOCH;
            persist_bonds(l);
            lg(l, 2, "sl2: dial echoes epochs — replay guard latched");
        }
        return true;
    }
    if (!(d->bond.flags & SL2_BOND_F_EPOCH)) return true;   /* legacy dial */
    /* Stale echo from a latched dial: replay, or the dial missed the STATE
     * after our reboot. Drop it, but resync the dial promptly so a live one
     * learns the fresh epoch instead of wedging. */
    d->pend_state = true;
    lg(l, 1, "sl2: stale epoch, packet dropped");
    return false;
}

/* ── STATE build + send ───────────────────────────────────────────────── */

static void build_state(sl2_link_t *l, struct sl2_state_pkt *p) {
    memset(p, 0, sizeof *p);
    p->type = SL2_PKT_STATE;
    p->version = SL2_PROTO_VERSION;
    p->caps_seq = l->caps_seq;
    p->epoch = l->epoch;
    sl2_hvac_state_t s;
    memset(&s, 0, sizeof s);
    s.set_low_dc = SL2_DC_NA; s.set_high_dc = SL2_DC_NA;
    s.room_hum_pct = SL2_HUM_NA; s.hum_set_pct = SL2_HUM_NA;
    if (!l->hvac->get_state(l->hvac->ctx, &s)) {
        /* adapter unavailable: honest empties, HVAC_LINK stays clear */
        p->set_low_dc = SL2_DC_NA; p->set_high_dc = SL2_DC_NA;
        p->room_hum_pct = SL2_HUM_NA; p->hum_set_pct = SL2_HUM_NA;
        return;
    }
    if (s.hvac_link)        p->flags |= SL2_SF_HVAC_LINK;
    if (s.wifi)             p->flags |= SL2_SF_WIFI;
    if (s.use_f)            p->flags |= SL2_SF_USE_F;
    if (s.wifi_provisioned) p->flags |= SL2_SF_WIFI_PROVISIONED;
    if (s.setup_ap)         p->flags |= SL2_SF_SETUP_AP;
    if (s.wifi_err)         p->flags |= SL2_SF_WIFI_ERR;
    if (s.sensor_batt_low) p->flags2 |= SL2_SF2_SENSOR_BATT_LOW;
    p->mode = s.mode; p->action = s.action; p->fan = s.fan;
    p->vane_v = s.vane_v; p->vane_h = s.vane_h; p->preset = s.preset;
    p->fault = s.fault;
    p->room_dc = s.room_dc; p->set_dc = s.set_dc;
    p->set_low_dc = s.set_low_dc; p->set_high_dc = s.set_high_dc;
    p->room_hum_pct = s.room_hum_pct; p->hum_set_pct = s.hum_set_pct;
}

static void send_state_to(sl2_link_t *l, sl2_dial_rt_t *d,
                          const struct sl2_state_pkt *p, uint32_t now) {
    l->port->send(l->port->ctx, d->bond.mac, p, sizeof *p);
    d->last_state_tx_ms = now;
    d->pend_state = false;
    d->want &= (uint8_t)~SL2_WANT_STATE;   /* any STATE satisfies a pull */
}

/* Immediate fan-out (post-CMD echo, post-pair sync). */
static void echo_state_all(sl2_link_t *l) {
    uint32_t now = l->port->now_ms(l->port->ctx);
    struct sl2_state_pkt p;
    build_state(l, &p);
    l->last_state = p;
    l->have_last_state = true;
    for (int i = 0; i < l->n_dials; i++) {
        sl2_dial_rt_t *d = &l->dial[i];
        if (dial_live_at(d, now)) send_state_to(l, d, &p, now);
        else d->pend_state = true;
    }
}

/* ── pairing ──────────────────────────────────────────────────────────── */

static void pair_cleanup_candidate(sl2_link_t *l) {
    sl2_dial_rt_t *bonded = dial_by_mac(l, l->cand_mac);
    l->port->peer_del(l->port->ctx, l->cand_mac);
    if (bonded)   /* re-pair attempt clobbered the radio peer: restore it */
        l->port->peer_add(l->port->ctx, bonded->bond.mac, bonded->bond.lmk, true);
    memset(l->cand_mac, 0, 6);
    memset(l->cand_lmk, 0, sizeof l->cand_lmk);
    memset(l->cand_id_pub, 0, sizeof l->cand_id_pub);
    memset(l->eph_priv, 0, sizeof l->eph_priv);
}

/* Common close-out for every pairing exit (paired, cancelled, timeout). The
 * broadcast peer exists only to carry PAIR_REQ/PAIR_RESP, so return the
 * radio's peer slot as soon as the window closes. */
static void pair_end(sl2_link_t *l, const char *result) {
    l->port->peer_del(l->port->ctx, SL2_BCAST_MAC);
    l->pair = SL2_PAIR_OFF;
    l->pair_result = result;
}

void sl2_link_pair_start(sl2_link_t *l, uint32_t window_ms) {
    if (!l->started) { l->pair_result = "idle"; return; }
    if (l->n_dials >= SL2_MAX_DIALS) { l->pair_result = "full"; return; }
    /* Already pairing (double button press, on_boot + button): only extend
     * the window. Regenerating the ephemeral or resetting CONFIRM here
     * killed a mid-flight handshake — the dial's confirming probe arrived
     * to a WINDOW state and never committed. */
    if (l->pair != SL2_PAIR_OFF) {
        l->pair_deadline_ms = l->port->now_ms(l->port->ctx) + window_ms;
        lg(l, 2, "sl2: pairing window extended");
        return;
    }
    if (l->crypto->x25519_keypair(l->crypto->ctx, l->eph_priv, l->eph_pub)) {
        l->pair_result = "idle";
        lg(l, 0, "sl2: eph keypair failed");
        return;
    }
    l->port->peer_add(l->port->ctx, SL2_BCAST_MAC, NULL, false);
    l->pair = SL2_PAIR_WINDOW;
    l->pair_deadline_ms = l->port->now_ms(l->port->ctx) + window_ms;
    l->pair_result = "listening";
    lg(l, 2, "sl2: pairing window open");
}

void sl2_link_pair_cancel(sl2_link_t *l) {
    if (l->pair == SL2_PAIR_OFF) return;
    if (l->pair == SL2_PAIR_CONFIRM) pair_cleanup_candidate(l);
    pair_end(l, "cancelled");
}

bool sl2_link_pairing(const sl2_link_t *l) { return l->pair != SL2_PAIR_OFF; }

int sl2_link_pair_seconds_left(sl2_link_t *l) {
    if (l->pair == SL2_PAIR_OFF) return 0;
    uint32_t now = l->port->now_ms(l->port->ctx);
    int32_t left = (int32_t)(l->pair_deadline_ms - now);
    return left > 0 ? (left + 999) / 1000 : 0;
}

const char *sl2_link_pair_result(const sl2_link_t *l) { return l->pair_result; }

static void on_pair_req(sl2_link_t *l, const uint8_t *data, int len) {
    if (l->pair == SL2_PAIR_OFF) return;
    if (len < SL2_PAIR_MIN_LEN) return;
    struct sl2_pair_req_pkt req;
    sl2_decode_pkt(&req, sizeof req, data, len);
    if (req.type != SL2_PKT_PAIR_REQ) return;

    /* During CONFIRM only re-answer the same candidate (it may have missed
     * our RESP); a different dial waits for the next window. */
    if (l->pair == SL2_PAIR_CONFIRM && !sl2_mac_eq(req.src_mac, l->cand_mac))
        return;

    uint8_t tr[SL2_REQ_TRANSCRIPT_LEN];
    sl2_pair_req_transcript(&req, tr);
    if (l->crypto->ed25519_verify(l->crypto->ctx, req.id_pub,
                                  tr, sizeof tr, req.sig) != 0) {
        lg(l, 1, "sl2: pair req bad signature");
        return;
    }

    /* TOFU pinning: a known dial MAC must present its pinned identity. */
    sl2_dial_rt_t *known = dial_by_mac(l, req.src_mac);
    if (known && memcmp(known->bond.id_pub, req.id_pub, 32) != 0) {
        l->pair_result = "pin-mismatch";
        lg(l, 1, "sl2: pair req identity != pinned identity, refusing");
        return;
    }
    if (!known && l->n_dials >= SL2_MAX_DIALS) {
        l->pair_result = "full";
        return;
    }

    uint8_t lmk[16];
    if (sl2_derive_lmk(l->crypto, l->eph_priv, req.eph_pub,
                       req.eph_pub /* dial */, l->eph_pub /* ctrl */, lmk)) {
        lg(l, 0, "sl2: lmk derivation failed");
        return;
    }

    struct sl2_pair_resp_pkt resp;
    memset(&resp, 0, sizeof resp);
    resp.type = SL2_PKT_PAIR_RESP;
    resp.version = SL2_PROTO_VERSION;
    memcpy(resp.src_mac, l->own_mac, 6);
    memcpy(resp.eph_pub, l->eph_pub, 32);
    memcpy(resp.id_pub, l->id_pub, 32);
    resp.channel = l->port->get_channel ? l->port->get_channel(l->port->ctx) : 0;
    uint8_t rtr[SL2_RESP_TRANSCRIPT_LEN];
    sl2_pair_resp_transcript(&resp, req.eph_pub, rtr);
    if (l->crypto->ed25519_sign(l->crypto->ctx, l->id_priv,
                                rtr, sizeof rtr, resp.sig)) {
        lg(l, 0, "sl2: resp sign failed");
        return;
    }

    memcpy(l->cand_mac, req.src_mac, 6);
    memcpy(l->cand_lmk, lmk, 16);
    memcpy(l->cand_id_pub, req.id_pub, 32);
    /* Install (or re-key) the candidate as an encrypted peer so its
     * confirming encrypted PROBE can reach us at all. A failed install is
     * fatal to the handshake and must be LOUD — the dial would otherwise
     * probe into a silent decrypt-drop for 6 s and report a bare timeout. */
    l->port->peer_del(l->port->ctx, l->cand_mac);
    if (!l->port->peer_add(l->port->ctx, l->cand_mac, l->cand_lmk, true)) {
        lg(l, 0, "sl2: candidate encrypted-peer install FAILED");
        memset(l->cand_lmk, 0, sizeof l->cand_lmk);
        return;
    }
    l->port->send(l->port->ctx, SL2_BCAST_MAC, &resp, sizeof resp);
    l->pair = SL2_PAIR_CONFIRM;
    l->confirm_deadline_ms = l->port->now_ms(l->port->ctx) + SL2_PAIR_CONFIRM_MS;
    l->pair_result = "confirming";
    memset(lmk, 0, sizeof lmk);
}

/* First encrypted unicast from the candidate proves both ends derived the
 * same LMK (the radio drops mismatched CCMP frames). Commit the bond. */
static void pair_commit(sl2_link_t *l) {
    sl2_dial_rt_t *d = dial_by_mac(l, l->cand_mac);
    if (!d) {
        d = &l->dial[l->n_dials++];
        memset(d, 0, sizeof *d);
    }
    memset(&d->bond, 0, sizeof d->bond);
    memcpy(d->bond.mac, l->cand_mac, 6);
    memcpy(d->bond.lmk, l->cand_lmk, 16);
    memcpy(d->bond.id_pub, l->cand_id_pub, 32);
    d->pend_state = true;
    persist_bonds(l);
    memset(l->cand_lmk, 0, sizeof l->cand_lmk);
    memset(l->eph_priv, 0, sizeof l->eph_priv);
    pair_end(l, "paired");
    lg(l, 2, "sl2: dial paired");
}

/* ── RX ───────────────────────────────────────────────────────────────── */

void sl2_link_on_recv(sl2_link_t *l, const uint8_t src[6], const uint8_t dst[6],
                      const uint8_t *data, int len) {
    if (!l->started || len < 2) return;
    const uint8_t type = data[0];
    const uint8_t ver  = data[1];

    if (type == SL2_PKT_PAIR_REQ) { on_pair_req(l, data, len); return; }

    /* Everything else: bonded unicast only. */
    if (!sl2_mac_eq(dst, l->own_mac)) return;

    /* Pairing confirmation: any unicast that decrypted from the candidate. */
    if (l->pair == SL2_PAIR_CONFIRM && sl2_mac_eq(src, l->cand_mac))
        pair_commit(l);

    sl2_dial_rt_t *d = dial_by_mac(l, src);
    if (!d) return;

    /* Version floor: PROBE bypasses so a skewed dial is still seen. */
    if (type != SL2_PKT_PROBE && ver < SL2_PROTO_MIN_COMPAT) return;
    uint32_t now = l->port->now_ms(l->port->ctx);

    switch (type) {
    case SL2_PKT_PROBE:
        if (len >= SL2_PROBE_MIN_LEN) {
            struct sl2_probe_pkt p;
            sl2_decode_pkt(&p, sizeof p, data, len);
            d->want = p.want;
        }
        d->last_probe_ms = now;
        break;
    case SL2_PKT_CMD:
        if (len >= SL2_CMD_MIN_LEN) {
            struct sl2_cmd_pkt c;
            sl2_decode_pkt(&c, sizeof c, data, len);
            if (!epoch_ok(l, d, c.epoch)) break;   /* replays don't prove liveness */
            d->last_probe_ms = now;         /* a command proves liveness too */
            if (l->hvac->apply(l->hvac->ctx, c.mask, &c))
                echo_state_all(l);          /* converge every head fast */
        }
        break;
    case SL2_PKT_WIFI_REQ:
        if (len >= SL2_WIFI_REQ_MIN_LEN) {
            d->wifi_req = true;
            d->last_probe_ms = now;
        }
        break;
    case SL2_PKT_WIFI_SETUP:
        if (len >= SL2_WIFI_SETUP_MIN_LEN) {
            struct sl2_wifi_setup_pkt w;
            sl2_decode_pkt(&w, sizeof w, data, len);
            if (!epoch_ok(l, d, w.epoch)) break;
            d->wifi_setup_req = true;
            d->last_probe_ms = now;
        }
        break;
    case SL2_PKT_DIAL_INFO:
        if (len >= SL2_DIAL_INFO_MIN_LEN) {
            struct sl2_dial_info_pkt di;
            sl2_decode_pkt(&di, sizeof di, data, len);
            d->peer_caps_seq = di.caps_seq;
            memcpy(d->model, di.model, sizeof d->model);
            d->model[sizeof d->model - 1] = 0;
            memcpy(d->fw, di.fw, sizeof d->fw);
            d->fw[sizeof d->fw - 1] = 0;
            d->have_info = true;
            d->last_probe_ms = now;
        }
        break;
    default:
        break;
    }
}

/* ── loop ─────────────────────────────────────────────────────────────── */

static void serve_pulls(sl2_link_t *l, sl2_dial_rt_t *d, uint32_t now) {
    if ((d->want & SL2_WANT_CAPS) &&
        (d->last_caps_tx_ms == 0 ||
         (uint32_t)(now - d->last_caps_tx_ms) >= SL2_PULL_THROTTLE_MS)) {
        struct sl2_caps_pkt caps;
        memset(&caps, 0, sizeof caps);
        if (l->hvac->get_caps(l->hvac->ctx, &caps)) {
            caps.type = SL2_PKT_CAPS;
            caps.version = SL2_PROTO_VERSION;
            caps.caps_seq = l->caps_seq;
            l->port->send(l->port->ctx, d->bond.mac, &caps, sizeof caps);
            d->last_caps_tx_ms = now;
        }
    }
    if ((d->want & SL2_WANT_INFO) &&
        (d->last_info_tx_ms == 0 ||
         (uint32_t)(now - d->last_info_tx_ms) >= SL2_PULL_THROTTLE_MS)) {
        uint8_t buf[SL2_MTU];
        struct sl2_info_pkt hdr = { SL2_PKT_INFO, SL2_PROTO_VERSION, {0, 0} };
        memcpy(buf, &hdr, SL2_INFO_HDR_LEN);
        size_t n = SL2_INFO_HDR_LEN;
        if (l->hvac->fill_info_tlvs)
            n += l->hvac->fill_info_tlvs(l->hvac->ctx, buf + n, sizeof buf - n);
        l->port->send(l->port->ctx, d->bond.mac, buf, n);
        d->last_info_tx_ms = now;
    }
    if (d->wifi_req) {
        d->wifi_req = false;
        struct sl2_wifi_resp_pkt r;
        memset(&r, 0, sizeof r);
        r.type = SL2_PKT_WIFI_RESP;
        r.version = SL2_PROTO_VERSION;
        if (l->hvac->wifi_creds &&
            l->hvac->wifi_creds(l->hvac->ctx, r.ssid, r.psk))
            r.ok = 1;
        l->port->send(l->port->ctx, d->bond.mac, &r, sizeof r);
        memset(&r, 0, sizeof r);            /* no PSK copy left behind */
    }
    if (d->wifi_setup_req) {
        d->wifi_setup_req = false;
        if (l->hvac->wifi_setup)
            l->hvac->wifi_setup(l->hvac->ctx);   /* AP status echoes back via STATE */
    }
}

void sl2_link_loop(sl2_link_t *l) {
    if (!l->started) return;
    uint32_t now = l->port->now_ms(l->port->ctx);

    /* pairing timeouts */
    if (l->pair == SL2_PAIR_WINDOW &&
        (int32_t)(now - l->pair_deadline_ms) >= 0) {
        pair_end(l, "timeout");
    } else if (l->pair == SL2_PAIR_CONFIRM &&
               ((int32_t)(now - l->confirm_deadline_ms) >= 0 ||
                (int32_t)(now - l->pair_deadline_ms) >= 0)) {
        pair_cleanup_candidate(l);
        pair_end(l, "timeout");
    }

    if (l->n_dials == 0) return;

    /* STATE only goes to live dials — skip the hvac read + change detection
     * entirely when none are (this loop runs every adapter tick). */
    bool any_live = false;
    for (int i = 0; i < l->n_dials; i++) {
        if (dial_live_at(&l->dial[i], now)) { any_live = true; break; }
        l->dial[i].was_live = false;
    }
    if (!any_live) return;

    /* shared STATE change detection */
    struct sl2_state_pkt p;
    build_state(l, &p);
    if (!l->have_last_state ||
        memcmp(&p, &l->last_state, sizeof p) != 0) {
        l->last_state = p;
        l->have_last_state = true;
        mark_all_state_pending(l);
    }

    for (int i = 0; i < l->n_dials; i++) {
        sl2_dial_rt_t *d = &l->dial[i];
        bool live = dial_live_at(d, now);
        if (!live) { d->was_live = false; continue; }
        bool first = !d->was_live;
        d->was_live = true;
        /* WANT_STATE is a dial-initiated pull (zone switch / resync): serve
         * it at the same rate floor as change-driven sends. The bit stays
         * set until a STATE actually goes out (send_state clears it), so a
         * pull landing inside the floor is served right after it passes;
         * loss retry is the dial re-probing with the bit still set. */
        bool pulled = (d->want & SL2_WANT_STATE) != 0;
        if (first ||
            ((d->pend_state || pulled) &&
             (uint32_t)(now - d->last_state_tx_ms) >= SL2_STATE_MIN_INTERVAL_MS) ||
            (uint32_t)(now - d->last_state_tx_ms) >= SL2_STATE_HEARTBEAT_MS) {
            send_state_to(l, d, &p, now);
        }
        serve_pulls(l, d, now);
    }
}

/* ── lifecycle / management ───────────────────────────────────────────── */

void sl2_link_init(sl2_link_t *l, const sl2_port_t *port,
                   const sl2_crypto_t *crypto, const sl2_hvac_iface_t *hvac) {
    memset(l, 0, sizeof *l);
    l->port = port;
    l->crypto = crypto;
    l->hvac = hvac;
    l->pair_result = "idle";
}

bool sl2_link_start(sl2_link_t *l) {
    if (!l->port->own_mac(l->port->ctx, l->own_mac)) return false;

    size_t len = SL2_ED25519_PRIV + SL2_ED25519_PUB;
    uint8_t id[SL2_ED25519_PRIV + SL2_ED25519_PUB];
    if (l->port->kv_get(l->port->ctx, SL2_KV_IDENTITY, id, &len) &&
        len == sizeof id) {
        memcpy(l->id_priv, id, SL2_ED25519_PRIV);
        memcpy(l->id_pub, id + SL2_ED25519_PRIV, SL2_ED25519_PUB);
    } else {
        if (l->crypto->ed25519_keypair(l->crypto->ctx, l->id_priv, l->id_pub))
            return false;
        memcpy(id, l->id_priv, SL2_ED25519_PRIV);
        memcpy(id + SL2_ED25519_PRIV, l->id_pub, SL2_ED25519_PUB);
        if (!l->port->kv_set(l->port->ctx, SL2_KV_IDENTITY, id, sizeof id))
            return false;
        lg(l, 2, "sl2: generated device identity");
    }
    memset(id, 0, sizeof id);

    uint8_t blob[SL2_BONDS_BLOB_MAX];
    len = sizeof blob;
    if (l->port->kv_get(l->port->ctx, SL2_KV_BONDS, blob, &len)) {
        sl2_dial_bond_t recs[SL2_MAX_DIALS];
        int n = sl2_bonds_decode(blob, len, recs);
        if (n > 0) {
            l->n_dials = n;
            for (int i = 0; i < n; i++) {
                memset(&l->dial[i], 0, sizeof l->dial[i]);
                l->dial[i].bond = recs[i];
                l->port->peer_add(l->port->ctx, recs[i].mac, recs[i].lmk, true);
            }
        }
    }

    len = 1;
    uint8_t seq = 0;
    if (l->port->kv_get(l->port->ctx, SL2_KV_CAPS_SEQ, &seq, &len) && len == 1)
        l->caps_seq = seq;

    /* Per-boot replay epoch (see epoch_ok). 0 is reserved for "no epoch", so
     * a rand failure honestly disables the guard rather than blocking dials. */
    uint8_t eb[2];
    if (l->crypto->rand_bytes(l->crypto->ctx, eb, 2) == 0) {
        l->epoch = (uint16_t)(eb[0] | ((uint16_t)eb[1] << 8));
        if (l->epoch == 0) l->epoch = 1;
    } else {
        l->epoch = 0;
        lg(l, 0, "sl2: epoch rand failed — replay guard off this boot");
    }

    l->started = true;
    return true;
}

int sl2_link_dial_count(const sl2_link_t *l) { return l->n_dials; }

bool sl2_link_dial_mac(const sl2_link_t *l, int idx, uint8_t out[6]) {
    if (idx < 0 || idx >= l->n_dials) return false;
    memcpy(out, l->dial[idx].bond.mac, 6);
    return true;
}

bool sl2_link_forget_dial(sl2_link_t *l, const uint8_t mac[6]) {
    for (int i = 0; i < l->n_dials; i++) {
        if (!sl2_mac_eq(l->dial[i].bond.mac, mac)) continue;
        l->port->peer_del(l->port->ctx, mac);
        for (int j = i; j < l->n_dials - 1; j++) l->dial[j] = l->dial[j + 1];
        l->n_dials--;
        memset(&l->dial[l->n_dials], 0, sizeof l->dial[l->n_dials]);
        persist_bonds(l);
        return true;
    }
    return false;
}

void sl2_link_forget_all(sl2_link_t *l) {
    for (int i = 0; i < l->n_dials; i++)
        l->port->peer_del(l->port->ctx, l->dial[i].bond.mac);
    memset(l->dial, 0, sizeof l->dial);
    l->n_dials = 0;
    persist_bonds(l);
}

bool sl2_link_dial_live(sl2_link_t *l, int idx) {
    if (idx < 0 || idx >= l->n_dials) return false;
    return dial_live_at(&l->dial[idx], l->port->now_ms(l->port->ctx));
}

bool sl2_link_any_live(sl2_link_t *l) {
    for (int i = 0; i < l->n_dials; i++)
        if (sl2_link_dial_live(l, i)) return true;
    return false;
}

bool sl2_link_dial_view(sl2_link_t *l, int idx, sl2_dial_view_t *out) {
    if (idx < 0 || idx >= l->n_dials) return false;
    const sl2_dial_rt_t *d = &l->dial[idx];
    uint32_t now = l->port->now_ms(l->port->ctx);
    memcpy(out->mac, d->bond.mac, 6);
    out->live = dial_live_at(d, now);
    out->last_seen_ms = d->last_probe_ms ? (int32_t)(now - d->last_probe_ms) : -1;
    out->have_info = d->have_info;
    memcpy(out->model, d->model, sizeof out->model);
    memcpy(out->fw, d->fw, sizeof out->fw);
    out->caps_seq = d->peer_caps_seq;
    return true;
}

uint8_t sl2_link_caps_seq(const sl2_link_t *l) { return l->caps_seq; }

void sl2_link_caps_changed(sl2_link_t *l) {
    l->caps_seq++;
    persist_caps_seq(l);
    mark_all_state_pending(l);   /* STATE carries the new seq -> dials re-pull */
}
