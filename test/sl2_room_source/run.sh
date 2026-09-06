#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_sl2_room_source' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_sl2_room_source.c -lm -o /tmp/test_sl2_room_source
/tmp/test_sl2_room_source
