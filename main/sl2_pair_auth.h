/* Key confirmation for v5 pairing. Shared by controller and dial. */
#pragma once
#include "sl2_proto.h"
#include "sl2_sha256.h"

/* The LMK is derived from the shared secret and BOTH fresh ephemeral keys.
 * Domain, direction, version and ordered MACs prevent cross-protocol/role use.
 * Authenticate the fixed v5 prefix; future extensions must define their own
 * authentication before assigning security meaning to trailing bytes. */
static inline void sl2_pair_auth_make(struct sl2_pair_auth_pkt *p, uint8_t type,
                                      const uint8_t lmk[16],
                                      const uint8_t dial_mac[6],
                                      const uint8_t ctrl_mac[6]) {
    uint8_t tr[28];
    memcpy(tr, "SLv5-pair-auth", 14); /* excludes the terminating NUL */
    p->type = type;
    p->version = SL2_PAIR_AUTH_MIN_VER;
    tr[14] = p->type;
    tr[15] = p->version;
    memcpy(tr + 16, dial_mac, 6);
    memcpy(tr + 22, ctrl_mac, 6);
    sl2_hmac_sha256(lmk, 16, tr, sizeof tr, p->tag);
}

/* The dial RX callback uses a precomputed expected ACK, so the Wi-Fi task
 * need not run crypto or read a key being installed elsewhere. */
static inline bool sl2_pair_auth_matches(const uint8_t *data, int len,
                                         const struct sl2_pair_auth_pkt *want) {
    if (len < SL2_PAIR_AUTH_MIN_LEN || data[0] != want->type ||
        data[1] != want->version) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof want->tag; i++) diff |= data[2 + i] ^ want->tag[i];
    return diff == 0;
}
