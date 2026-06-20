#include "espnow_crypto.h"
#include "espnow_proto.h"
#include <string.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

static int rng_init(mbedtls_ctr_drbg_context *drbg, mbedtls_entropy_context *ent) {
    mbedtls_entropy_init(ent);
    mbedtls_ctr_drbg_init(drbg);
    static const char *pers = "serin-espnow-pair";
    return mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, ent,
                                 (const unsigned char *)pers, strlen(pers));
}

/* RFC 7748 X25519 scalar clamping. mbedtls does NOT clamp for us — its
 * ecp_mul/ecdh reject an unclamped scalar with MBEDTLS_ERR_ECP_INVALID_KEY. */
static void x25519_clamp(uint8_t k[32]) {
    k[0]  &= 0xF8;
    k[31] &= 0x7F;
    k[31] |= 0x40;
}

int espnow_crypto_pub_from_priv(const uint8_t priv[32], uint8_t pub[32]) {
    mbedtls_ecp_group grp; mbedtls_mpi d; mbedtls_ecp_point Q;
    mbedtls_ctr_drbg_context drbg; mbedtls_entropy_context ent;
    int rc;
    uint8_t k[32];
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&Q);
    if ((rc = rng_init(&drbg, &ent)) != 0) goto out;
    if ((rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519)) != 0) goto out;
    /* X25519 scalars are little-endian and must be RFC 7748 clamped before use. */
    memcpy(k, priv, 32); x25519_clamp(k);
    if ((rc = mbedtls_mpi_read_binary_le(&d, k, 32)) != 0) goto out;
    if ((rc = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, mbedtls_ctr_drbg_random, &drbg)) != 0) goto out;
    if ((rc = mbedtls_mpi_write_binary_le(&Q.MBEDTLS_PRIVATE(X), pub, 32)) != 0) goto out;
    rc = 0;
out:
    mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&d); mbedtls_ecp_point_free(&Q);
    mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
    return rc;
}

int espnow_crypto_keypair(uint8_t priv[32], uint8_t pub[32]) {
    mbedtls_ctr_drbg_context drbg; mbedtls_entropy_context ent;
    int e = rng_init(&drbg, &ent);
    if (e != 0) { mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent); return e; }
    int rc = mbedtls_ctr_drbg_random(&drbg, priv, 32);
    mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
    if (rc != 0) return rc;
    x25519_clamp(priv);   /* store a clamped scalar so derive_lmk uses a valid key */
    return espnow_crypto_pub_from_priv(priv, pub);
}

int espnow_crypto_derive_lmk(const uint8_t own_priv[32], const uint8_t peer_pub[32],
                             const uint8_t dial_pub[32], const uint8_t unit_pub[32],
                             uint8_t lmk[16]) {
    mbedtls_ecp_group grp; mbedtls_mpi d, z; mbedtls_ecp_point P;
    mbedtls_ctr_drbg_context drbg; mbedtls_entropy_context ent;
    uint8_t shared[32], salt[64], k[32];
    int rc = -1;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&P);
    if (rng_init(&drbg, &ent) != 0) goto out;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto out;
    memcpy(k, own_priv, 32); x25519_clamp(k);
    if (mbedtls_mpi_read_binary_le(&d, k, 32) != 0) goto out;
    if (mbedtls_mpi_read_binary_le(&P.MBEDTLS_PRIVATE(X), peer_pub, 32) != 0) goto out;
    if (mbedtls_mpi_lset(&P.MBEDTLS_PRIVATE(Z), 1) != 0) goto out;
    if (mbedtls_ecdh_compute_shared(&grp, &z, &P, &d,
                                    mbedtls_ctr_drbg_random, &drbg) != 0) goto out;
    if (mbedtls_mpi_write_binary_le(&z, shared, 32) != 0) goto out;
    espnow_pair_salt(dial_pub, unit_pub, salt);
    if (mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                     salt, sizeof(salt), shared, sizeof(shared),
                     (const unsigned char *)"serin-espnow-lmk-v1", 19,
                     lmk, ESPNOW_LMK_LEN) != 0) goto out;
    rc = 0;
out:
    mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&d); mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&P);
    mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
    memset(shared, 0, sizeof(shared));
    return rc;
}

void espnow_crypto_auth_tag(const uint8_t *pmk, size_t pmk_len,
                            const uint8_t *msg, size_t msg_len, uint8_t tag[16]) {
    uint8_t full[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    pmk, pmk_len, msg, msg_len, full);
    memcpy(tag, full, 16);
}

bool espnow_crypto_tag_ok(const uint8_t a[16], const uint8_t b[16]) {
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}
