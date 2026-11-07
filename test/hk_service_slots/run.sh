#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_hk_service_slots' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_hk_service_slots.cpp -o /tmp/test_hk_service_slots
/tmp/test_hk_service_slots
