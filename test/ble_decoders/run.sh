#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_ble_decoders' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_ble_decoders.cpp -lm -o /tmp/test_ble_decoders
/tmp/test_ble_decoders
