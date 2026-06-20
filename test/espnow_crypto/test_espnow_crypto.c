#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "espnow_crypto.h"

/* Fixed (already-clamped) X25519 test scalars — RFC 7748 §6.1 vectors. */
static const uint8_t ALICE_PRIV[32] = {
    0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
    0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a };
static const uint8_t BOB_PRIV[32] = {
    0x5d,0xab,0x08,0x7e,0x62,0x4a,0x8a,0x4b,0x79,0xe1,0x7f,0x8b,0x83,0x80,0x0e,0xe6,
    0x6f,0x3b,0xb1,0x29,0x26,0x18,0xb6,0xfd,0x1c,0x2f,0x8b,0x27,0xff,0x88,0xe0,0xeb };

int main(void) {
    uint8_t a_pub[32], b_pub[32];
    assert(espnow_crypto_pub_from_priv(ALICE_PRIV, a_pub) == 0);
    assert(espnow_crypto_pub_from_priv(BOB_PRIV,   b_pub) == 0);

    /* Dial role: own=ALICE, peer=BOB.  Unit role: own=BOB, peer=ALICE.
     * dial_pub=a_pub, unit_pub=b_pub for BOTH (same salt order). */
    uint8_t lmk_dial[16], lmk_unit[16];
    assert(espnow_crypto_derive_lmk(ALICE_PRIV, b_pub, a_pub, b_pub, lmk_dial) == 0);
    assert(espnow_crypto_derive_lmk(BOB_PRIV,   a_pub, a_pub, b_pub, lmk_unit) == 0);
    assert(memcmp(lmk_dial, lmk_unit, 16) == 0);            /* both derive same LMK */

    uint8_t zero[16] = {0};
    assert(memcmp(lmk_dial, zero, 16) != 0);                /* not trivially zero */

    const uint8_t pmk[16] = "TESTVECTORpmk000";  /* arbitrary 16-byte test input, not the brand PMK */
    uint8_t msg[40]; for (int i = 0; i < 40; i++) msg[i] = (uint8_t)i;
    uint8_t t1[16], t2[16];
    espnow_crypto_auth_tag(pmk, 16, msg, sizeof(msg), t1);
    espnow_crypto_auth_tag(pmk, 16, msg, sizeof(msg), t2);
    assert(espnow_crypto_tag_ok(t1, t2));                   /* deterministic */
    msg[0] ^= 0x01;
    espnow_crypto_auth_tag(pmk, 16, msg, sizeof(msg), t2);
    assert(!espnow_crypto_tag_ok(t1, t2));                  /* tamper detected */

    printf("ALL CRYPTO TESTS PASSED\n");
    return 0;
}
