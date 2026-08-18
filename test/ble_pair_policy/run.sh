#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_ble_pair_policy' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_ble_pair_policy.cpp -o /tmp/test_ble_pair_policy
/tmp/test_ble_pair_policy
