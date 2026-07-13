#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_event_ring' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_event_ring.cpp -o /tmp/test_event_ring
/tmp/test_event_ring
