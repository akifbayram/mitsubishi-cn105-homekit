/*
 * espnow_crypto.h — pairing crypto for the Dial<->unit link.
 * Dependency-free interface; implementation uses mbedTLS (X25519 + HKDF-SHA256
 * + HMAC-SHA256). Vendored byte-identical into main/ and the Dial main/.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_X25519_LEN 32
#define ESPNOW_LMK_LEN    16
#define ESPNOW_TAG_LEN    16

/* Fresh ephemeral X25519 keypair (priv clamped per RFC 7748). 0 on success. */
int espnow_crypto_keypair(uint8_t priv[32], uint8_t pub[32]);

/* X25519 base point * priv -> pub. Deterministic. 0 on success. */
int espnow_crypto_pub_from_priv(const uint8_t priv[32], uint8_t pub[32]);

/* shared = X25519(own_priv, peer_pub); lmk = HKDF-SHA256(shared,
 * salt = dial_pub||unit_pub, info="serin-espnow-lmk-v1")[:16]. 0 on success. */
int espnow_crypto_derive_lmk(const uint8_t own_priv[32], const uint8_t peer_pub[32],
                             const uint8_t dial_pub[32], const uint8_t unit_pub[32],
                             uint8_t lmk[16]);

/* tag = HMAC-SHA256(pmk, msg)[:16]. */
void espnow_crypto_auth_tag(const uint8_t *pmk, size_t pmk_len,
                            const uint8_t *msg, size_t msg_len, uint8_t tag[16]);

/* constant-time 16-byte compare; true if equal. */
bool espnow_crypto_tag_ok(const uint8_t a[16], const uint8_t b[16]);

#ifdef __cplusplus
}
#endif
