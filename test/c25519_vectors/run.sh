#!/usr/bin/env bash
# Wire-compatibility proof for the compact Ed25519/X25519 swap: compiles the
# REAL shims (main/sl2_sodium_slim.c) plus vendored Monocypher for the host
# and checks them against RFC 8032 / RFC 7748 vectors. See host_test.c.
set -euo pipefail
trap 'rm -f /tmp/test_c25519_vectors' EXIT
cd "$(dirname "$0")"
gcc -std=c11 -O2 -Wall -Wextra -Werror -I../../main \
    host_test.c ../../main/sl2_sodium_slim.c ../../main/sl2_monocypher/*.c \
    -o /tmp/test_c25519_vectors
/tmp/test_c25519_vectors
