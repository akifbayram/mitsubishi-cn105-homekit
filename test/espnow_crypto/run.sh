#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if ! echo '#include <mbedtls/hkdf.h>
int main(void){return 0;}' | gcc -x c - -lmbedcrypto -o /tmp/_mbedprobe 2>/dev/null; then
  echo "SKIP: host mbedTLS (libmbedtls-dev) not available; crypto verified on-device via espnow-selftest"
  exit 0
fi
rm -f /tmp/_mbedprobe
trap 'rm -f /tmp/test_espnow_crypto' EXIT
gcc -std=c11 -Wall -Wextra -I../../main \
    test_espnow_crypto.c ../../main/espnow_crypto.c -lmbedcrypto -o /tmp/test_espnow_crypto
/tmp/test_espnow_crypto
