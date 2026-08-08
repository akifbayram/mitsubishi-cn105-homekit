#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_room_feed' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_room_feed.c -lm -o /tmp/test_room_feed
/tmp/test_room_feed
