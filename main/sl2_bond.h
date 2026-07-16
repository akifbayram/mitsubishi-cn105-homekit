/*
 * sl2_bond.h — controller-side dial-bond table: pure codec, host-testable.
 * Storage (via the port's kv hooks) wraps this; the blob layout is pinned by
 * the host tests. fmt=2 distinguishes from the v1 single-bond layout.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL2_MAX_DIALS 4
#define SL2_BOND_FMT  2

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t lmk[16];
    uint8_t id_pub[32];     /* pinned dial Ed25519 identity */
    uint8_t flags;          /* SL2_BOND_F_*; pre-flag blobs decode as 0 */
    uint8_t reserved[7];    /* zero-fill on write, ignore on read */
} sl2_dial_bond_t;

/* This dial has echoed a correct STATE.epoch at least once — it understands
 * the replay guard, so zero/stale epochs from it are now rejected. Ratchets
 * on (persisted); re-pairing the dial resets it with the rest of the bond. */
#define SL2_BOND_F_EPOCH 0x01

#define SL2_BOND_REC_SIZE 62
#define SL2_BONDS_BLOB_MAX (2 + SL2_MAX_DIALS * SL2_BOND_REC_SIZE)
typedef char sl2_bond_sa[(sizeof(sl2_dial_bond_t) == SL2_BOND_REC_SIZE) ? 1 : -1];

/* Encode n records. Returns blob length, or 0 if cap is too small / n out of
 * range. Layout: u8 fmt; u8 count; recs[count]. */
static inline size_t sl2_bonds_encode(const sl2_dial_bond_t *recs, int n,
                                      uint8_t *blob, size_t cap) {
    if (n < 0 || n > SL2_MAX_DIALS) return 0;
    size_t need = 2 + (size_t)n * SL2_BOND_REC_SIZE;
    if (cap < need) return 0;
    blob[0] = SL2_BOND_FMT;
    blob[1] = (uint8_t)n;
    if (n) memcpy(blob + 2, recs, (size_t)n * SL2_BOND_REC_SIZE);
    return need;
}

/* Decode into out[SL2_MAX_DIALS]. Returns record count, or -1 on a malformed
 * blob (wrong fmt, impossible count, short data). */
static inline int sl2_bonds_decode(const uint8_t *blob, size_t len,
                                   sl2_dial_bond_t *out) {
    if (len < 2 || blob[0] != SL2_BOND_FMT) return -1;
    int n = blob[1];
    if (n > SL2_MAX_DIALS) return -1;
    if (len < 2 + (size_t)n * SL2_BOND_REC_SIZE) return -1;
    if (n) memcpy(out, blob + 2, (size_t)n * SL2_BOND_REC_SIZE);
    return n;
}

#ifdef __cplusplus
}
#endif
