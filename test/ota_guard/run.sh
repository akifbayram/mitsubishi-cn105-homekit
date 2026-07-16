#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_ota_guard' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_ota_guard.cpp -o /tmp/test_ota_guard
/tmp/test_ota_guard
