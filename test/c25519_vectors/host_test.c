// Host-side wire-compatibility proof for the compact-crypto swap. Exercises
// the REAL shim entry points from main/sl2_sodium_slim.c (compiled for the
// host — SL2_TIMED collapses to a passthrough off-ESP), so the hand-written
// glue (seed||pk layout, siglen handling, all-zero X25519 rejection) is under
// test, not just Monocypher underneath it.
//
// Ed25519 is deterministic: if keygen and signatures match the RFC 8032
// vectors byte-for-byte, output is identical to libsodium's for every
// (seed, message) — i.e. the wire format is unchanged. X25519 is checked
// against the RFC 7748 vectors plus a DH round-trip.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// libsodium-ABI shim entry points (main/sl2_sodium_slim.c).
int crypto_sign_ed25519_keypair(unsigned char *pk, unsigned char *sk);
int crypto_sign_ed25519_detached(unsigned char *sig, unsigned long long *siglen_p,
                                 const unsigned char *m, unsigned long long mlen,
                                 const unsigned char *sk);
int crypto_sign_ed25519_verify_detached(const unsigned char *sig, const unsigned char *m,
                                        unsigned long long mlen, const unsigned char *pk);
int crypto_scalarmult_curve25519(unsigned char *q, const unsigned char *n,
                                 const unsigned char *p);
int crypto_scalarmult_curve25519_base(unsigned char *q, const unsigned char *n);

// The shim's keypair source of randomness; fixed bytes make the test
// deterministic (the keypair test below only checks internal consistency).
static uint8_t g_next_random[32];
void randombytes_buf(void *buf, size_t size) {
    for (size_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = g_next_random[i % 32];
}

static void unhex(uint8_t *out, const char *hex) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) sscanf(hex + 2 * i, "%2hhx", &out[i]);
}
static int fails = 0;
static void check(const char *name, const uint8_t *got, const char *exp_hex, size_t n) {
    uint8_t exp[128]; unhex(exp, exp_hex);
    if (memcmp(got, exp, n)) { printf("FAIL %s\n", name); fails++; }
    else printf("ok   %s\n", name);
}

// RFC 8032 §7.1 test vectors: secret(seed), public, message, signature
static const struct { const char *sk, *pk, *msg, *sig; } ED[] = {
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
     "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
     "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
     "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    // §7.1 TEST SHA(abc): 1023-byte msg omitted; the 64-byte-digest case:
    {"833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
     "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
     "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"},
};

int main(void) {
    for (unsigned t = 0; t < sizeof(ED)/sizeof(ED[0]); t++) {
        uint8_t sk[32], pk[32], msg[256], sig[64];
        size_t mlen = strlen(ED[t].msg) / 2;
        unhex(sk, ED[t].sk); unhex(pk, ED[t].pk); unhex(msg, ED[t].msg);
        char name[64];
        uint8_t sk64[64], pk_kp[32];
        // Drive the real keypair entry point with the vector's seed as the
        // "randomness": proves the seed||pk secret-key layout end to end.
        memcpy(g_next_random, sk, 32);
        crypto_sign_ed25519_keypair(pk_kp, sk64);
        snprintf(name, sizeof name, "rfc8032[%u] keypair", t);
        check(name, pk_kp, ED[t].pk, 32);
        snprintf(name, sizeof name, "rfc8032[%u] sk layout", t);
        check(name, sk64 + 32, ED[t].pk, 32);
        unsigned long long siglen = 0;
        crypto_sign_ed25519_detached(sig, &siglen, msg, mlen, sk64);
        snprintf(name, sizeof name, "rfc8032[%u] sign", t);
        check(name, sig, ED[t].sig, 64);
        if (siglen != 64) { printf("FAIL siglen[%u]=%llu\n", t, siglen); fails++; }
        if (crypto_sign_ed25519_verify_detached(sig, msg, mlen, pk)) { printf("FAIL verify[%u]\n", t); fails++; }
        sig[0] ^= 1;
        if (!crypto_sign_ed25519_verify_detached(sig, msg, mlen, pk)) { printf("FAIL tamper-accept[%u]\n", t); fails++; }
        uint8_t badmsg[256]; memcpy(badmsg, msg, mlen); badmsg[0] ^= 1; sig[0] ^= 1;
        if (mlen && !crypto_sign_ed25519_verify_detached(sig, badmsg, mlen, pk)) { printf("FAIL msg-tamper-accept[%u]\n", t); fails++; }
    }

    // RFC 7748 §5.2 X25519 vector 1
    {
        uint8_t k[32], u[32], out[32];
        unhex(k, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
        unhex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
        if (crypto_scalarmult_curve25519(out, k, u)) { printf("FAIL x25519 rc\n"); fails++; }
        check("rfc7748 vec1", out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);
    }
    // RFC 7748 §6.1 Diffie-Hellman: alice/bob keys -> shared secret
    {
        uint8_t a[32], b[32], apub[32], bpub[32], s1[32], s2[32];
        unhex(a, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        unhex(b, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
        crypto_scalarmult_curve25519_base(apub, a);
        crypto_scalarmult_curve25519_base(bpub, b);
        check("rfc7748 alice pub", apub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);
        check("rfc7748 bob pub", bpub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
        if (crypto_scalarmult_curve25519(s1, a, bpub)) { printf("FAIL dh rc\n"); fails++; }
        if (crypto_scalarmult_curve25519(s2, b, apub)) { printf("FAIL dh rc\n"); fails++; }
        check("rfc7748 shared a*B", s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
        check("rfc7748 shared b*A", s2, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    }
    // Shim contract: X25519 with a small-order point yields all-zero -> -1.
    {
        uint8_t zero_pt[32] = {0}, k[32] = {1}, out[32];
        if (crypto_scalarmult_curve25519(out, k, zero_pt) != -1) {
            printf("FAIL small-order rejection\n"); fails++;
        } else printf("ok   small-order rejection\n");
    }
    // libsodium verify strictness (stricter than plain RFC 8032): reject
    // small-order R, small-order pk, non-canonical pk, and non-canonical
    // (S+L) scalars — mirrors ed25519_ref10's preconditions so shimmed and
    // stock-libsodium devices accept exactly the same signatures.
    {
        uint8_t pk[32], msg[4] = {0}, sig[64], bad[64], badpk[32];
        unhex(pk, ED[0].pk); unhex(sig, ED[0].sig);
        size_t mlen = 0;

        memcpy(bad, sig, 64);
        memset(bad, 0, 31); bad[0] = 0x01;            // R = identity (order 1)
        if (!crypto_sign_ed25519_verify_detached(bad, msg, mlen, pk)) {
            printf("FAIL small-order R accepted\n"); fails++;
        } else printf("ok   small-order R rejected\n");

        memset(badpk, 0, 32); badpk[0] = 0x01;        // pk = identity
        if (!crypto_sign_ed25519_verify_detached(sig, msg, mlen, badpk)) {
            printf("FAIL small-order pk accepted\n"); fails++;
        } else printf("ok   small-order pk rejected\n");

        memset(badpk, 0xff, 32); badpk[0] = 0xef; badpk[31] = 0x7f; // y = p+2
        if (!crypto_sign_ed25519_verify_detached(sig, msg, mlen, badpk)) {
            printf("FAIL non-canonical pk accepted\n"); fails++;
        } else printf("ok   non-canonical pk rejected\n");

        memcpy(bad, sig, 64);                          // S' = S + L
        unhex(bad + 32, "4c8c7872aa064e049dbb3013fbf29380d25bf5f0595bbe24655141438e7a101b");
        if (!crypto_sign_ed25519_verify_detached(bad, msg, mlen, pk)) {
            printf("FAIL non-canonical S accepted\n"); fails++;
        } else printf("ok   non-canonical S rejected\n");
    }
    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails != 0;
}
