/* Host tests for this controller's vendored Serin Link core at protocol v5:
 * drives main/sl2_link.c through a fake port (captured sends, fake clock,
 * in-memory kv) with the same deterministic toy crypto the core's own tests
 * use. Pins what the firmware relies on across the v4 -> v5 bump:
 *   - a v5 handshake (PAIR_REQ, PAIR_RESP, authenticated PAIR_CONFIRM) saves
 *     the bond and answers PAIR_ACK;
 *   - a v4 PAIR_REQ is ignored and nothing is saved;
 *   - a format-2 "sl2_dials" blob written by the v4 firmware still loads;
 *   - a failed kv_set during commit yields "storage-error" and leaves the
 *     saved bonds byte-identical. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sl2_link.h"
#include "sl2_pair_auth.h"

/* ── fake port ────────────────────────────────────────────────────────── */

#define MAX_SENDS 32
typedef struct { uint8_t mac[6]; uint8_t data[250]; size_t len; } sent_t;
typedef struct { uint8_t mac[6]; uint8_t lmk[16]; bool encrypt; bool present; } fpeer_t;
typedef struct { char key[16]; uint8_t val[256]; size_t len; bool present; } fkv_t;

static struct {
    uint32_t now;
    sent_t sent[MAX_SENDS];
    int n_sent;
    fpeer_t peers[8];
    fkv_t kv[8];
    uint8_t own[6];
    bool fail_bond_writes;
    int bond_write_attempts;
    int storage_errors;
} F;

static void f_reset(void) { memset(&F, 0, sizeof F); memset(F.own, 0xC0, 6); }

static bool f_send(void *c, const uint8_t mac[6], const void *buf, size_t len) {
    (void)c;
    if (F.n_sent >= MAX_SENDS || len > 250) return false;
    sent_t *s = &F.sent[F.n_sent++];
    memcpy(s->mac, mac, 6);
    memcpy(s->data, buf, len);
    s->len = len;
    return true;
}
static fpeer_t *f_find_peer(const uint8_t mac[6]) {
    for (int i = 0; i < 8; i++)
        if (F.peers[i].present && memcmp(F.peers[i].mac, mac, 6) == 0)
            return &F.peers[i];
    return NULL;
}
static bool f_peer_add(void *c, const uint8_t mac[6], const uint8_t lmk[16], bool enc) {
    (void)c;
    fpeer_t *p = f_find_peer(mac);
    if (!p) {
        for (int i = 0; i < 8 && !p; i++) if (!F.peers[i].present) p = &F.peers[i];
        if (!p) return false;
    }
    p->present = true;
    memcpy(p->mac, mac, 6);
    if (lmk) memcpy(p->lmk, lmk, 16); else memset(p->lmk, 0, 16);
    p->encrypt = enc;
    return true;
}
static void f_peer_del(void *c, const uint8_t mac[6]) {
    (void)c;
    fpeer_t *p = f_find_peer(mac);
    if (p) p->present = false;
}
static bool f_own_mac(void *c, uint8_t out[6]) { (void)c; memcpy(out, F.own, 6); return true; }
static uint8_t f_channel(void *c) { (void)c; return 6; }
static uint32_t f_now(void *c) { (void)c; return F.now; }
static fkv_t *f_kv_find(const char *k) {
    for (int i = 0; i < 8; i++)
        if (F.kv[i].present && strcmp(F.kv[i].key, k) == 0) return &F.kv[i];
    return NULL;
}
static bool f_kv_get(void *c, const char *k, void *buf, size_t *len) {
    (void)c;
    fkv_t *s = f_kv_find(k);
    if (!s) return false;
    size_t n = s->len < *len ? s->len : *len;
    memcpy(buf, s->val, n);
    *len = s->len;
    return true;
}
/* Mirrors the NVS contract in sl2_port.h: a failed write leaves the previous
 * value in place. */
static bool f_kv_set(void *c, const char *k, const void *buf, size_t len) {
    (void)c;
    if (strcmp(k, SL2_KV_BONDS) == 0) {
        F.bond_write_attempts++;
        if (F.fail_bond_writes) return false;
    }
    if (len > 256) return false;
    fkv_t *slot = f_kv_find(k);
    for (int i = 0; i < 8 && !slot; i++) if (!F.kv[i].present) slot = &F.kv[i];
    assert(slot);
    slot->present = true;
    snprintf(slot->key, sizeof slot->key, "%s", k);
    memcpy(slot->val, buf, len);
    slot->len = len;
    return true;
}
static void f_log(void *c, int level, const char *msg) {
    (void)c;
    if (level == 0 && strstr(msg, "save failed")) F.storage_errors++;
}
static const sl2_port_t FPORT = {
    .ctx = NULL, .send = f_send, .peer_add = f_peer_add, .peer_del = f_peer_del,
    .own_mac = f_own_mac, .get_channel = f_channel, .now_ms = f_now,
    .kv_get = f_kv_get, .kv_set = f_kv_set, .log = f_log,
};

/* ── toy crypto (deterministic, invertible — FSM tests only) ──────────── */

static uint8_t t_ctr = 1;
static int t_rand(void *c, uint8_t *b, size_t n) {
    (void)c;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(t_ctr + i);
    t_ctr += 7;
    return 0;
}
static int t_xkp(void *c, uint8_t priv[32], uint8_t pub[32]) {
    t_rand(c, priv, 32);
    for (int i = 0; i < 32; i++) pub[i] = priv[i] ^ 0xAA;
    return 0;
}
static int t_xsh(void *c, const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]) {
    (void)c;
    for (int i = 0; i < 32; i++) out[i] = (priv[i] ^ 0xAA) ^ peer[i];
    return 0;
}
static int t_ekp(void *c, uint8_t priv[64], uint8_t pub[32]) {
    t_rand(c, priv, 32);
    for (int i = 0; i < 32; i++) pub[i] = priv[i] ^ 0x55;
    memcpy(priv + 32, pub, 32);
    return 0;
}
static void t_sig_of(const uint8_t seed[32], const uint8_t *m, size_t ml, uint8_t sig[64]) {
    for (int i = 0; i < 64; i++)
        sig[i] = (uint8_t)(seed[i % 32] ^ m[i % ml] ^ (uint8_t)ml ^ i);
}
static int t_sign(void *c, const uint8_t priv[64], const uint8_t *m, size_t ml, uint8_t sig[64]) {
    (void)c;
    t_sig_of(priv, m, ml, sig);
    return 0;
}
static int t_verify(void *c, const uint8_t pub[32], const uint8_t *m, size_t ml, const uint8_t sig[64]) {
    (void)c;
    uint8_t seed[32], want[64];
    for (int i = 0; i < 32; i++) seed[i] = pub[i] ^ 0x55;
    t_sig_of(seed, m, ml, want);
    return memcmp(want, sig, 64) == 0 ? 0 : -1;
}
static const sl2_crypto_t FCRYPTO = {
    .ctx = NULL, .rand_bytes = t_rand, .x25519_keypair = t_xkp,
    .x25519_shared = t_xsh,
    .ed25519_keypair = t_ekp, .ed25519_sign = t_sign, .ed25519_verify = t_verify,
};

/* ── minimal HVAC adapter: the optional hooks are NULL ────────────────── */

static sl2_hvac_state_t H;
static bool h_get_state(void *c, sl2_hvac_state_t *out) { (void)c; *out = H; return true; }
static bool h_apply(void *c, uint16_t mask, const struct sl2_cmd_pkt *cmd) {
    (void)c; (void)mask; (void)cmd;
    return true;
}
static bool h_get_caps(void *c, struct sl2_caps_pkt *out) {
    (void)c;
    out->modes = (1u << SL2_MODE_OFF) | (1u << SL2_MODE_HEAT);
    out->fan_steps = 5;
    out->set_min_dc = 160; out->set_max_dc = 305; out->set_step_dc = 5;
    snprintf(out->name, sizeof out->name, "Bench");
    return true;
}
static size_t h_tlvs(void *c, uint8_t *buf, size_t cap) { (void)c; (void)buf; (void)cap; return 0; }
static const sl2_hvac_iface_t FHVAC = {
    .ctx = NULL, .get_state = h_get_state, .apply = h_apply,
    .get_caps = h_get_caps, .fill_info_tlvs = h_tlvs,
};

/* ── dial-side simulation ─────────────────────────────────────────────── */

typedef struct {
    uint8_t mac[6];
    uint8_t id_priv[64], id_pub[32];
    uint8_t eph_priv[32], eph_pub[32];
    uint8_t lmk[16];
} fdial_t;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void dial_make(fdial_t *d, uint8_t tag) {
    memset(d, 0, sizeof *d);
    memset(d->mac, tag, 6);
    t_ekp(NULL, d->id_priv, d->id_pub);
    t_xkp(NULL, d->eph_priv, d->eph_pub);
}

static void dial_req(const fdial_t *d, uint8_t version, struct sl2_pair_req_pkt *req) {
    memset(req, 0, sizeof *req);
    req->type = SL2_PKT_PAIR_REQ;
    req->version = version;
    memcpy(req->src_mac, d->mac, 6);
    memcpy(req->eph_pub, d->eph_pub, 32);
    memcpy(req->id_pub, d->id_pub, 32);
    uint8_t tr[SL2_REQ_TRANSCRIPT_LEN];
    sl2_pair_req_transcript(req, tr);
    t_sign(NULL, d->id_priv, tr, sizeof tr, req->sig);
}

static int last_send_of(uint8_t type) {
    for (int i = F.n_sent - 1; i >= 0; i--)
        if (F.sent[i].len >= 1 && F.sent[i].data[0] == type) return i;
    return -1;
}
static int count_sends_of(uint8_t type) {
    int n = 0;
    for (int i = 0; i < F.n_sent; i++)
        if (F.sent[i].len >= 1 && F.sent[i].data[0] == type) n++;
    return n;
}

/* PAIR_REQ in, PAIR_RESP out; the dial verifies it and derives its LMK. */
static void dial_request_and_accept(sl2_link_t *l, fdial_t *d) {
    int sends_before = F.n_sent;
    struct sl2_pair_req_pkt req;
    dial_req(d, SL2_PROTO_VERSION, &req);
    sl2_link_on_recv(l, d->mac, BCAST, (const uint8_t *)&req, (int)sizeof req);
    int ri = last_send_of(SL2_PKT_PAIR_RESP);
    assert(ri >= sends_before);
    struct sl2_pair_resp_pkt resp;
    sl2_decode_pkt(&resp, sizeof resp, F.sent[ri].data, (int)F.sent[ri].len);
    uint8_t rt[SL2_RESP_TRANSCRIPT_LEN];
    sl2_pair_resp_transcript(&resp, d->eph_pub, rt);
    assert(t_verify(NULL, resp.id_pub, rt, sizeof rt, resp.sig) == 0);
    assert(sl2_derive_lmk(&FCRYPTO, d->eph_priv, resp.eph_pub,
                          d->eph_pub, resp.eph_pub, d->lmk) == 0);
}

static void dial_confirm(sl2_link_t *l, const fdial_t *d) {
    struct sl2_pair_auth_pkt p;
    sl2_pair_auth_make(&p, SL2_PKT_PAIR_CONFIRM, d->lmk, d->mac, F.own);
    sl2_link_on_recv(l, d->mac, F.own, (const uint8_t *)&p, sizeof p);
}

static void fresh(sl2_link_t *l) {
    f_reset();
    memset(&H, 0, sizeof H);
    H.hvac_link = true;
    H.mode = SL2_MODE_HEAT;
    H.room_dc = 210; H.set_dc = 220;
    H.set_low_dc = SL2_DC_NA; H.set_high_dc = SL2_DC_NA;
    H.room_hum_pct = 40; H.hum_set_pct = SL2_HUM_NA;
    F.now = 1000;
    sl2_link_init(l, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(l));
}

static void reboot(sl2_link_t *l) {
    memset(F.peers, 0, sizeof F.peers);
    F.n_sent = 0;
    sl2_link_init(l, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(l));
}

/* Decode the saved bond table; returns record count (-1 if none/malformed). */
static int saved_bonds(sl2_dial_bond_t out[SL2_MAX_DIALS]) {
    fkv_t *s = f_kv_find(SL2_KV_BONDS);
    if (!s) return -1;
    return sl2_bonds_decode(s->val, s->len, out);
}

/* ── tests ────────────────────────────────────────────────────────────── */

static void test_v5_pairing_saves_bond(void) {
    sl2_link_t l;
    fresh(&l);
    assert(saved_bonds(NULL) == -1);          /* nothing bonded yet */
    fdial_t d;
    dial_make(&d, 0xD1);
    sl2_link_pair_start(&l, 60000);
    assert(sl2_link_pairing(&l));
    dial_request_and_accept(&l, &d);
    assert(sl2_link_dial_count(&l) == 0);     /* RESP alone commits nothing */
    assert(saved_bonds(NULL) == -1);

    dial_confirm(&l, &d);
    assert(!sl2_link_pairing(&l));
    assert(strcmp(sl2_link_pair_result(&l), "paired") == 0);
    assert(sl2_link_dial_count(&l) == 1);

    /* Saved under the unchanged key, in the unchanged format, with the v5
     * replay-guard latch set from the start. */
    sl2_dial_bond_t recs[SL2_MAX_DIALS];
    assert(saved_bonds(recs) == 1);
    assert(memcmp(recs[0].mac, d.mac, 6) == 0);
    assert(memcmp(recs[0].lmk, d.lmk, 16) == 0);
    assert(memcmp(recs[0].id_pub, d.id_pub, 32) == 0);
    assert(recs[0].flags & SL2_BOND_F_EPOCH);

    /* The dial got an authenticated PAIR_ACK under the new key. */
    struct sl2_pair_auth_pkt ack;
    sl2_pair_auth_make(&ack, SL2_PKT_PAIR_ACK, d.lmk, d.mac, F.own);
    int ai = last_send_of(SL2_PKT_PAIR_ACK);
    assert(ai >= 0 && memcmp(F.sent[ai].mac, d.mac, 6) == 0);
    assert(sl2_pair_auth_matches(F.sent[ai].data, (int)F.sent[ai].len, &ack));
    fpeer_t *p = f_find_peer(d.mac);
    assert(p && p->encrypt && memcmp(p->lmk, d.lmk, 16) == 0);

    /* Survives a reboot from storage alone. */
    sl2_link_t l2;
    reboot(&l2);
    uint8_t mac[6];
    assert(sl2_link_dial_count(&l2) == 1 && sl2_link_dial_mac(&l2, 0, mac));
    assert(memcmp(mac, d.mac, 6) == 0);
    p = f_find_peer(d.mac);
    assert(p && p->encrypt && memcmp(p->lmk, d.lmk, 16) == 0);
    printf("v5 pairing saves bond ok\n");
}

static void test_v4_pair_request_is_ignored(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d;
    dial_make(&d, 0xD2);
    sl2_link_pair_start(&l, 60000);
    struct sl2_pair_req_pkt req;
    dial_req(&d, 4, &req);                    /* well-formed, signed, v4 */
    sl2_link_on_recv(&l, d.mac, BCAST, (const uint8_t *)&req, (int)sizeof req);
    assert(sl2_link_pairing(&l));             /* window still open */
    assert(strcmp(sl2_link_pair_result(&l), "listening") == 0);
    assert(last_send_of(SL2_PKT_PAIR_RESP) < 0);
    assert(!f_find_peer(d.mac));
    assert(sl2_link_dial_count(&l) == 0);
    assert(F.bond_write_attempts == 0);
    assert(saved_bonds(NULL) == -1);

    /* Even a correct candidate-key proof cannot follow a refused request. */
    uint8_t zero_lmk[16] = {0};
    memcpy(d.lmk, zero_lmk, 16);
    dial_confirm(&l, &d);
    assert(sl2_link_dial_count(&l) == 0 && saved_bonds(NULL) == -1);

    F.now += 60001;
    sl2_link_loop(&l);
    assert(strcmp(sl2_link_pair_result(&l), "timeout") == 0);
    assert(saved_bonds(NULL) == -1);
    printf("v4 pair request ignored ok\n");
}

/* A record exactly as the v4 firmware wrote it: fmt 2, one 62-byte record,
 * flags 0 (the replay guard had not latched), reserved zero-filled. Built by
 * hand so the test proves the layout, not that the codec agrees with itself. */
static size_t legacy_blob(uint8_t blob[64], const uint8_t mac[6],
                          const uint8_t lmk[16], const uint8_t id_pub[32]) {
    memset(blob, 0, 64);
    blob[0] = 2;                              /* fmt */
    blob[1] = 1;                              /* count */
    memcpy(blob + 2, mac, 6);
    memcpy(blob + 8, lmk, 16);
    memcpy(blob + 24, id_pub, 32);
    blob[56] = 0;                             /* flags */
    return 64;
}

static void test_legacy_format2_blob_still_loads(void) {
    /* Storage contract the hard rules pin. */
    assert(strcmp(SL2_KV_BONDS, "sl2_dials") == 0);
    assert(strcmp(SL2_KV_IDENTITY, "sl2_id") == 0);
    assert(SL2_BOND_FMT == 2 && SL2_BOND_REC_SIZE == 62);

    f_reset();
    uint8_t mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
    uint8_t lmk[16], id_pub[32];
    for (int i = 0; i < 16; i++) lmk[i] = (uint8_t)(0x10 + i);
    for (int i = 0; i < 32; i++) id_pub[i] = (uint8_t)(0x80 + i);
    uint8_t blob[64];
    size_t len = legacy_blob(blob, mac, lmk, id_pub);
    assert(f_kv_set(NULL, SL2_KV_BONDS, blob, len));
    F.bond_write_attempts = 0;

    sl2_link_t l;
    memset(&H, 0, sizeof H);
    H.hvac_link = true;
    F.now = 1000;
    sl2_link_init(&l, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l));

    uint8_t got[6];
    assert(sl2_link_dial_count(&l) == 1 && sl2_link_dial_mac(&l, 0, got));
    assert(memcmp(got, mac, 6) == 0);
    fpeer_t *p = f_find_peer(mac);
    assert(p && p->encrypt && memcmp(p->lmk, lmk, 16) == 0);
    assert(memcmp(l.dial[0].bond.id_pub, id_pub, 32) == 0);
    assert(l.dial[0].bond.flags == 0);        /* legacy grace preserved */

    /* Loading is read-only: the blob on "flash" is untouched. */
    assert(F.bond_write_attempts == 0);
    fkv_t *s = f_kv_find(SL2_KV_BONDS);
    assert(s && s->len == len && memcmp(s->val, blob, len) == 0);

    /* The old bond still carries traffic: a PROBE from it marks the dial
     * live and the next loop pass serves it a STATE. */
    struct sl2_probe_pkt probe = { SL2_PKT_PROBE, SL2_PROTO_VERSION, SL2_WANT_STATE, {0} };
    F.n_sent = 0;
    sl2_link_on_recv(&l, mac, F.own, (const uint8_t *)&probe, (int)sizeof probe);
    assert(sl2_link_dial_live(&l, 0));
    sl2_link_loop(&l);
    int si = last_send_of(SL2_PKT_STATE);
    assert(si >= 0 && memcmp(F.sent[si].mac, mac, 6) == 0);
    printf("legacy format-2 blob loads ok\n");
}

static void test_storage_error_leaves_saved_bonds_unchanged(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t keep, cand;
    dial_make(&keep, 0xD3);
    sl2_link_pair_start(&l, 60000);
    dial_request_and_accept(&l, &keep);
    dial_confirm(&l, &keep);
    assert(strcmp(sl2_link_pair_result(&l), "paired") == 0);
    fkv_t *s = f_kv_find(SL2_KV_BONDS);
    assert(s);
    uint8_t before[256];
    size_t before_len = s->len;
    memcpy(before, s->val, before_len);
    int acks = count_sends_of(SL2_PKT_PAIR_ACK);
    int attempts = F.bond_write_attempts;

    dial_make(&cand, 0xD4);
    sl2_link_pair_start(&l, 60000);
    dial_request_and_accept(&l, &cand);
    F.fail_bond_writes = true;
    F.now += 100;
    dial_confirm(&l, &cand);

    assert(strcmp(sl2_link_pair_result(&l), "storage-error") == 0);
    assert(!sl2_link_pairing(&l));
    assert(F.bond_write_attempts == attempts + 1);   /* the save was tried */
    assert(F.storage_errors == 1);                  /* ...and logged */
    assert(count_sends_of(SL2_PKT_PAIR_ACK) == acks); /* no success ACK */
    assert(sl2_link_dial_count(&l) == 1);
    uint8_t mac[6];
    assert(sl2_link_dial_mac(&l, 0, mac) && memcmp(mac, keep.mac, 6) == 0);
    assert(!f_find_peer(cand.mac));
    fpeer_t *p = f_find_peer(keep.mac);
    assert(p && memcmp(p->lmk, keep.lmk, 16) == 0);

    /* The saved table is byte-identical, and that is what the next boot sees. */
    s = f_kv_find(SL2_KV_BONDS);
    assert(s && s->len == before_len && memcmp(s->val, before, before_len) == 0);
    sl2_link_t l2;
    reboot(&l2);
    assert(sl2_link_dial_count(&l2) == 1);
    assert(sl2_link_dial_mac(&l2, 0, mac) && memcmp(mac, keep.mac, 6) == 0);
    assert(!f_find_peer(cand.mac));

    /* Storage healthy again: the same dial pairs on the next attempt. */
    F.fail_bond_writes = false;
    sl2_link_pair_start(&l2, 60000);
    dial_request_and_accept(&l2, &cand);
    dial_confirm(&l2, &cand);
    assert(strcmp(sl2_link_pair_result(&l2), "paired") == 0);
    assert(sl2_link_dial_count(&l2) == 2);
    printf("storage-error leaves saved bonds unchanged ok\n");
}

int main(void) {
    test_v5_pairing_saves_bond();
    test_v4_pair_request_is_ignored();
    test_legacy_format2_blob_still_loads();
    test_storage_error_leaves_saved_bonds_unchanged();
    printf("sl2_pair_v5: all tests passed\n");
    return 0;
}
