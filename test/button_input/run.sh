#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_button_input' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_button_input.cpp -o /tmp/test_button_input
/tmp/test_button_input
