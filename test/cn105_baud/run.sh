#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_cn105_baud' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_cn105_baud.cpp -o /tmp/test_cn105_baud
/tmp/test_cn105_baud
