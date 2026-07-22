/*
 * sl2_dial_cert.h — controller-side view of the Serin device certificate a
 * dial presents in the SL2_TLV_DIAL_CERT tail of DIAL_INFO (wire spec §10c):
 * the Serin root's Ed25519 signature over the dial's identity pubkey +
 * metadata. Layout mirrors the dial firmware's link_cert.h and is wire
 * contract; kept out of sl2_proto.h so that header stays byte-identical
 * across every vendored copy.
 *
 * Pure codec — signature checking goes through the adopter's sl2_crypto_t
 * (see dial_cert_apply in sl2_link.c).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL2_DIAL_CERT_FMT         1
#define SL2_DIAL_CERT_LEN         112
#define SL2_DIAL_CERT_SIGNED_LEN  48    /* bytes covered by sig */

typedef struct __attribute__((packed)) {
    uint8_t  fmt;             /* SL2_DIAL_CERT_FMT */
    uint8_t  root_key_id;     /* 1 = first Serin root */
    uint8_t  model;           /* 1=link15, 2=link21 */
    uint8_t  hw_rev;
    uint32_t serial;
    uint16_t issue_date;      /* days since 2026-01-01 */
    uint8_t  reserved[6];
    uint8_t  device_pub[32];  /* the dial's Ed25519 identity pubkey */
    uint8_t  sig[64];         /* Ed25519 by the Serin root over bytes 0..47 */
} sl2_dial_cert_t;

typedef char sl2_dial_cert_sa[(sizeof(sl2_dial_cert_t) == SL2_DIAL_CERT_LEN) ? 1 : -1];

/* Layout checks only (strict length, known fmt); signature is the caller's. */
static inline bool sl2_dial_cert_decode(const uint8_t *buf, size_t len,
                                        sl2_dial_cert_t *out) {
    if (len != SL2_DIAL_CERT_LEN) return false;
    memcpy(out, buf, SL2_DIAL_CERT_LEN);
    return out->fmt == SL2_DIAL_CERT_FMT;
}

#ifdef __cplusplus
}
#endif
