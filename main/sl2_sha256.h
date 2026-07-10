/*
 * sl2_sha256.h — self-contained SHA-256, HMAC-SHA256 and HKDF-SHA256.
 *
 * The Serin Link LMK derivation MUST be bit-exact across every adapter, so it
 * cannot depend on a platform crypto library. Two ESP-IDF builds with the
 * SAME libsodium version and the SAME CONFIG_LIBSODIUM_USE_MBEDTLS_SHA=y were
 * observed (2026-07-10) to produce DIFFERENT HMAC-SHA256 output for
 * byte-identical key+message — dial prk=98f76a72, controller prk=80fcea59 —
 * silently yielding mismatched LMKs and an un-decryptable link. This header
 * removes that whole class of divergence: everyone derives with this code.
 *
 * Public-domain style SHA-256; HMAC per RFC 2104; HKDF per RFC 5869. Pinned
 * by RFC 5869 test vectors in test/test_sl2_proto.c.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL2_SHA256_LEN   32
#define SL2_SHA256_BLOCK 64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sl2_sha256_ctx;

static const uint32_t sl2__k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

static inline uint32_t sl2__ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static inline void sl2__sha256_block(sl2_sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sl2__ror(w[i - 15], 7) ^ sl2__ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sl2__ror(w[i - 2], 17) ^ sl2__ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sl2__ror(e, 6) ^ sl2__ror(e, 11) ^ sl2__ror(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + sl2__k[i] + w[i];
        uint32_t S0 = sl2__ror(a, 2) ^ sl2__ror(a, 13) ^ sl2__ror(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static inline void sl2_sha256_init(sl2_sha256_ctx *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0; c->buflen = 0;
}

static inline void sl2_sha256_update(sl2_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8u;
    while (len) {
        size_t n = SL2_SHA256_BLOCK - c->buflen;
        if (n > len) n = len;
        memcpy(c->buf + c->buflen, p, n);
        c->buflen += n; p += n; len -= n;
        if (c->buflen == SL2_SHA256_BLOCK) {
            sl2__sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static inline void sl2_sha256_final(sl2_sha256_ctx *c, uint8_t out[32]) {
    uint64_t bl = c->bitlen;
    uint8_t pad = 0x80;
    sl2_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) sl2_sha256_update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bl >> (56 - 8 * i));
    /* bypass update's bitlen accounting for the length block */
    memcpy(c->buf + c->buflen, len, 8);
    sl2__sha256_block(c, c->buf);
    c->buflen = 0;
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

static inline void sl2_sha256(const void *data, size_t len, uint8_t out[32]) {
    sl2_sha256_ctx c;
    sl2_sha256_init(&c);
    sl2_sha256_update(&c, data, len);
    sl2_sha256_final(&c, out);
}

/* HMAC-SHA256 (RFC 2104). Keys longer than the 64-byte block are hashed. */
static inline void sl2_hmac_sha256(const uint8_t *key, size_t key_len,
                                   const uint8_t *msg, size_t msg_len,
                                   uint8_t out[32]) {
    uint8_t k[SL2_SHA256_BLOCK], ipad[SL2_SHA256_BLOCK], opad[SL2_SHA256_BLOCK];
    uint8_t inner[32];
    memset(k, 0, sizeof k);
    if (key_len > SL2_SHA256_BLOCK) sl2_sha256(key, key_len, k);
    else if (key_len) memcpy(k, key, key_len);
    for (int i = 0; i < SL2_SHA256_BLOCK; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    sl2_sha256_ctx c;
    sl2_sha256_init(&c);
    sl2_sha256_update(&c, ipad, sizeof ipad);
    sl2_sha256_update(&c, msg, msg_len);
    sl2_sha256_final(&c, inner);
    sl2_sha256_init(&c);
    sl2_sha256_update(&c, opad, sizeof opad);
    sl2_sha256_update(&c, inner, sizeof inner);
    sl2_sha256_final(&c, out);
    memset(k, 0, sizeof k); memset(ipad, 0, sizeof ipad);
    memset(opad, 0, sizeof opad); memset(inner, 0, sizeof inner);
}

/* HKDF-SHA256 (RFC 5869): extract + expand. out_len <= 255*32. */
static inline void sl2_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                                   const uint8_t *salt, size_t salt_len,
                                   const uint8_t *info, size_t info_len,
                                   uint8_t *out, size_t out_len) {
    uint8_t prk[32];
    uint8_t zero[32];
    memset(zero, 0, sizeof zero);
    sl2_hmac_sha256(salt_len ? salt : zero, salt_len ? salt_len : sizeof zero,
                    ikm, ikm_len, prk);
    uint8_t t[32];
    size_t t_len = 0, done = 0;
    uint8_t ctr = 1;
    while (done < out_len) {
        /* T(n) = HMAC(prk, T(n-1) || info || n) */
        uint8_t block[32 + 255 + 1];
        size_t bl = 0;
        if (t_len) { memcpy(block, t, t_len); bl += t_len; }
        if (info_len) { memcpy(block + bl, info, info_len); bl += info_len; }
        block[bl++] = ctr;
        sl2_hmac_sha256(prk, sizeof prk, block, bl, t);
        t_len = sizeof t;
        size_t n = out_len - done < t_len ? out_len - done : t_len;
        memcpy(out + done, t, n);
        done += n;
        ctr++;
        memset(block, 0, sizeof block);
    }
    memset(prk, 0, sizeof prk);
    memset(t, 0, sizeof t);
}

#ifdef __cplusplus
}
#endif
