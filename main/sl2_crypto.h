/*
 * sl2_crypto.h — crypto hooks for the Serin Link core.
 *
 * The core never calls a crypto library directly; adopters supply these hooks.
 * Reference bindings: libsodium (espressif/libsodium managed component — the
 * Serin controller already depends on it; note mbedTLS has NO Ed25519, so the
 * dial also binds libsodium for signatures while keeping mbedTLS for X25519/
 * HKDF if it prefers). All functions return 0 on success.
 *
 * Ed25519 secret keys use the libsodium convention: 64 bytes = seed || pubkey.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sl2_sha256.h"   /* HKDF is pinned in-tree, NOT a platform hook */

#ifdef __cplusplus
extern "C" {
#endif

#define SL2_X25519_LEN     32
#define SL2_ED25519_PUB    32
#define SL2_ED25519_PRIV   64   /* libsodium secret-key format */
#define SL2_ED25519_SIG    64
#define SL2_LMK_LEN        16

typedef struct sl2_crypto {
    void *ctx;
    int (*rand_bytes)(void *ctx, uint8_t *buf, size_t len);
    /* X25519 (RFC 7748; implementations clamp priv) */
    int (*x25519_keypair)(void *ctx, uint8_t priv[32], uint8_t pub[32]);
    int (*x25519_shared)(void *ctx, const uint8_t priv[32],
                         const uint8_t peer_pub[32], uint8_t shared[32]);
    /* NB: there is deliberately NO hkdf hook — see sl2_sha256.h. Two builds
     * of the same libsodium disagreed on streaming HMAC-SHA256, silently
     * producing mismatched LMKs. The KDF is pinned in-tree. */
    /* Ed25519 */
    int (*ed25519_keypair)(void *ctx, uint8_t priv[64], uint8_t pub[32]);
    int (*ed25519_sign)(void *ctx, const uint8_t priv[64],
                        const uint8_t *msg, size_t msg_len, uint8_t sig[64]);
    int (*ed25519_verify)(void *ctx, const uint8_t pub[32],
                          const uint8_t *msg, size_t msg_len,
                          const uint8_t sig[64]);   /* 0 = valid */
} sl2_crypto_t;

/* Derive the per-bond LMK; same call on both ends (pass the two EPHEMERAL
 * pubs in protocol order: dial first). Returns 0 on success. */
static inline int sl2_derive_lmk(const sl2_crypto_t *c,
                                 const uint8_t own_eph_priv[32],
                                 const uint8_t peer_eph_pub[32],
                                 const uint8_t dial_eph_pub[32],
                                 const uint8_t ctrl_eph_pub[32],
                                 uint8_t lmk[16]) {
    uint8_t shared[32], salt[64];
    int rc = c->x25519_shared(c->ctx, own_eph_priv, peer_eph_pub, shared);
    if (rc) return rc;
    /* sl2_lmk_salt() lives in sl2_proto.h; inline here to keep this header
     * independent: salt = dial_eph_pub || ctrl_eph_pub. */
    for (int i = 0; i < 32; i++) { salt[i] = dial_eph_pub[i]; salt[32 + i] = ctrl_eph_pub[i]; }
    sl2_hkdf_sha256(shared, 32, salt, 64,
                    (const uint8_t *)"serin-link-v2-lmk", 17, lmk, 16);
    memset(shared, 0, sizeof shared);
    return 0;
}

#ifdef __cplusplus
}
#endif
