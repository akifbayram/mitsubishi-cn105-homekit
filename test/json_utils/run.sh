#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_json_utils' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_json_utils.cpp -o /tmp/test_json_utils
/tmp/test_json_utils
