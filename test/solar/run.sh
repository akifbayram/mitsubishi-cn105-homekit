#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_solar' EXIT
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I../../main -DSOLAR_SELFTEST \
    ../../main/solar.cpp -lm -o /tmp/test_solar
/tmp/test_solar
