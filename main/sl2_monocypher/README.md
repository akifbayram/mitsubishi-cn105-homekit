# Monocypher (Ed25519/SHA-512 + X25519 subset) — flash-budget swap

From https://github.com/LoupVaillant/Monocypher @ 1830c06d (CC0 / BSD-2 dual),
files src/monocypher.{c,h} and src/optional/monocypher-ed25519.{c,h}, unmodified.

Linked on every target: main/sl2_sodium_slim.c shims libsodium's
crypto_sign_ed25519_* / crypto_scalarmult_curve25519* entry points onto
crypto_ed25519_* (the RFC 8032 SHA-512 variant — byte-identical signatures,
same seed||pk key layout) and crypto_x25519, serving both HomeKit
pair-setup/verify and Serin Link. Verified against RFC 8032/7748 vectors by
test/c25519_vectors/run.sh.

History: the first swap attempt used Daniel Beer's c25519 (8-bit limbs). It was
wire-correct but too slow on the 160 MHz C6 — Ed25519 verify took ~970 ms and
Serin Link pairing timed out on the Dial side. Monocypher's 32-bit-limb field
arithmetic is roughly an order of magnitude faster while staying table-free.
Unused Monocypher primitives (BLAKE2b, ChaCha, Argon2, …) are dropped by
--gc-sections; only the EdDSA/X25519/SHA-512 core links.
