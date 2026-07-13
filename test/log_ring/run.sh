#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_log_ring' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_log_ring.cpp -o /tmp/test_log_ring
/tmp/test_log_ring
