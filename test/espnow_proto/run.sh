#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_espnow_proto' EXIT
cd "$(dirname "$0")"
gcc -std=c11 -Wall -Wextra -Werror -I../../main \
    test_espnow_proto.c -lm -o /tmp/test_espnow_proto
/tmp/test_espnow_proto
