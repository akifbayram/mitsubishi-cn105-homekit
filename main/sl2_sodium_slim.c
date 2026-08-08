// Link-size trim for libsodium. Where and why this file is enabled is each
// repo's main/CMakeLists.txt's story (target gating, which callers exist);
// this file is repo-neutral and byte-identical across the firmwares that
// share it, and stays host-compilable so test/c25519_vectors/ can exercise
// the real entry points.
//
// Two mechanisms:
//
// 1. Selector stubs. sodium_init() unconditionally calls the runtime
//    implementation selector of every primitive family libsodium ships
//    (argon2, blake2b, aegis, chacha20, salsa20, poly1305, X25519), which
//    drags every family's objects into the image whether or not anything
//    calls them. The linker is passed --wrap=<selector> for each (see
//    main/sl2_sodium_slim.cmake), so the calls resolve to the stubs below and
//    unused families are never pulled from the archive. The stubs are safe
//    for families that ARE linked (e.g. ChaCha20-Poly1305 where HAP uses it):
//    on this target each family's vtable is statically initialized to its
//    ref implementation — the selectors exist to pick SSE/AVX variants on
//    x86 and are functional no-ops here.
//
// 2. Compact Ed25519/X25519. libsodium's ed25519_ref10 costs ~61 KB of
//    flash, 30 KB of which is a precomputed base-point table; these ops run
//    only at pairing/session-establishment, so the table-free Monocypher
//    implementation (main/sl2_monocypher/) is swapped in behind libsodium's
//    exact entry points. crypto_ed25519_* is the RFC 8032 SHA-512 variant:
//    deterministic, byte-identical signatures, same seed||pk secret-key
//    layout — wire- and NVS-compatible both ways. Proven against RFC
//    8032/7748 vectors by test/c25519_vectors/run.sh. (First attempt used
//    c25519, which paired too slowly — see sl2_monocypher/README.md.)
//
// Because this object is force-extracted at link start (the -u flags in
// sl2_sodium_slim.cmake), its definitions are registered before any archive
// scan, so libsodium's sign.c/keypair.c/ref10 objects are never pulled and
// no duplicate-symbol conflict can arise. If a libsodium bump adds a
// selector call, the build fails with an undefined __wrap_ symbol instead of
// silently growing — add the stub here and the symbol in the .cmake list.

#include <stdint.h>
#include <string.h>

#include "sl2_monocypher/monocypher-ed25519.h"
#include "sl2_monocypher/monocypher.h"

#ifdef ESP_PLATFORM
// Compile the shims against libsodium's real prototypes so any ABI drift is a
// device-build compile error (the host test declares them itself instead).
#include <sodium/crypto_scalarmult_curve25519.h>
#include <sodium/crypto_sign_ed25519.h>

#include "esp_log.h"
#include "esp_timer.h"
// Per-op timing, visible when the build's maximum log level admits DEBUG.
// Kept because implementation speed is load-bearing: the c25519 attempt was
// wire-correct yet broke pairing purely on latency (EXPERIMENT-REPORT.md).
#define SL2_TIMED(op)                                                     \
    do {                                                                  \
        int64_t t0_ = esp_timer_get_time();                               \
        op;                                                               \
        ESP_LOGD("sl2crypto", "%s: %lld ms", __func__,                    \
                 (long long)((esp_timer_get_time() - t0_) / 1000));       \
    } while (0)
#else
#define SL2_TIMED(op) do { op; } while (0)
#endif

// Declared here rather than via <sodium/randombytes.h> so the file compiles
// on the host, where the test provides its own implementation.
void randombytes_buf(void *buf, size_t size);

int crypto_sign_ed25519_keypair(unsigned char *pk, unsigned char *sk)
{
    uint8_t seed[32];
    randombytes_buf(seed, 32);
    SL2_TIMED(crypto_ed25519_key_pair(sk, pk, seed)); // wipes seed itself
    return 0;
}

int crypto_sign_ed25519_detached(unsigned char *sig, unsigned long long *siglen_p,
                                 const unsigned char *m, unsigned long long mlen,
                                 const unsigned char *sk)
{
    SL2_TIMED(crypto_ed25519_sign(sig, sk, m, (size_t)mlen));
    if (siglen_p) {
        *siglen_p = 64;
    }
    return 0;
}

// libsodium's verify is stricter than RFC 8032: it rejects small-order R and
// pk points and non-canonical pk encodings up front (ed25519_ref10.c
// ge25519_has_small_order / ge25519_is_canonical, mirrored here verbatim so
// shimmed and stock-libsodium devices accept exactly the same signatures).
// Monocypher covers the remaining precondition, canonical S (S < L) — proven
// by the high-S vector in test/c25519_vectors/host_test.c.
static int ed25519_pt_has_small_order(const unsigned char s[32])
{
    static const unsigned char blacklist[][32] = {
        /* 0 (order 4) */
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        /* 1 (order 1) */
        { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        /* 2707385501144840649318225287225658788936804267575313519463743609750303402022
           (order 8) */
        { 0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4,
          0x89, 0xf2, 0xef, 0x98, 0xf0, 0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6,
          0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05 },
        /* 55188659117513257062467267217118295137698188065244968500265048394206261417927
           (order 8) */
        { 0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b,
          0x76, 0x0d, 0x10, 0x67, 0x0f, 0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39,
          0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a },
        /* p-1 (order 2) */
        { 0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },
        /* p (=0, order 4) */
        { 0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },
        /* p+1 (=1, order 1) */
        { 0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f }
    };
    unsigned char c[7] = { 0 };
    unsigned int  k;
    size_t        i, j;

    for (j = 0; j < 31; j++) {
        for (i = 0; i < sizeof blacklist / sizeof blacklist[0]; i++) {
            c[i] |= s[j] ^ blacklist[i][j];
        }
    }
    for (i = 0; i < sizeof blacklist / sizeof blacklist[0]; i++) {
        c[i] |= (s[j] & 0x7f) ^ blacklist[i][j];
    }
    k = 0;
    for (i = 0; i < sizeof blacklist / sizeof blacklist[0]; i++) {
        k |= (unsigned int)(c[i] - 1);
    }
    return (int)((k >> 8) & 1);
}

static int ed25519_pt_is_canonical(const unsigned char s[32])
{
    unsigned char c;
    unsigned char d;
    unsigned int  i;

    c = (s[31] & 0x7f) ^ 0x7f;
    for (i = 30; i > 0; i--) {
        c |= s[i] ^ 0xff;
    }
    c = (((unsigned int) c) - 1U) >> 8;
    d = (0xed - 1U - (unsigned int) s[0]) >> 8;

    return 1 - (c & d & 1);
}

int crypto_sign_ed25519_verify_detached(const unsigned char *sig, const unsigned char *m,
                                        unsigned long long mlen, const unsigned char *pk)
{
    if (ed25519_pt_has_small_order(sig) != 0 ||          // R
        ed25519_pt_is_canonical(pk) == 0 ||
        ed25519_pt_has_small_order(pk) != 0) {
        return -1;
    }
    int bad;
    SL2_TIMED(bad = crypto_ed25519_check(sig, pk, m, (size_t)mlen));
    return bad ? -1 : 0;
}

int crypto_scalarmult_curve25519(unsigned char *q, const unsigned char *n,
                                 const unsigned char *p)
{
    // crypto_x25519 clamps the scalar and masks the u high bit per RFC 7748,
    // matching libsodium's decoding exactly.
    SL2_TIMED(crypto_x25519(q, n, p));
    // libsodium contract: reject an all-zero shared secret (small-order
    // input). Hand-rolled rather than sodium_is_zero() to stay
    // host-compilable without libsodium headers.
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) {
        acc |= q[i];
    }
    return acc ? 0 : -1;
}

int crypto_scalarmult_curve25519_base(unsigned char *q, const unsigned char *n)
{
    SL2_TIMED(crypto_x25519_public_key(q, n));
    return 0;
}

// Selector stubs (mechanism 1). Keep in sync with sl2_sodium_slim.cmake.
int __wrap__crypto_pwhash_argon2_pick_best_implementation(void) { return 0; }
int __wrap__crypto_scalarmult_curve25519_pick_best_implementation(void) { return 0; }
int __wrap__crypto_generichash_blake2b_pick_best_implementation(void) { return 0; }
int __wrap__crypto_onetimeauth_poly1305_pick_best_implementation(void) { return 0; }
int __wrap__crypto_stream_chacha20_pick_best_implementation(void) { return 0; }
int __wrap__crypto_stream_salsa20_pick_best_implementation(void) { return 0; }
int __wrap__crypto_aead_aegis128l_pick_best_implementation(void) { return 0; }
int __wrap__crypto_aead_aegis256_pick_best_implementation(void) { return 0; }
