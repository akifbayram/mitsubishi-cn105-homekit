#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build=$(mktemp -d /tmp/test-setpoint-range.XXXXXX)
trap 'rm -rf "$build"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -I../../main \
    test_setpoint_range.cpp ../../main/setpoint_range.cpp -o "$build/test_setpoint_range"
"$build/test_setpoint_range" "$@"
