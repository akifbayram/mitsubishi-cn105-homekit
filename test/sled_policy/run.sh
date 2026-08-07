#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_sled_policy' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_sled_policy.cpp -o /tmp/test_sled_policy
/tmp/test_sled_policy
