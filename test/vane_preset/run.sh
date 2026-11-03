#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_vane_preset' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_vane_preset.cpp -o /tmp/test_vane_preset
/tmp/test_vane_preset
