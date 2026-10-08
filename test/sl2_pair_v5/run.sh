#!/usr/bin/env bash
set -euo pipefail
trap 'rm -f /tmp/test_sl2_pair_v5' EXIT
cd "$(dirname "$0")"
gcc -std=c11 -Wall -Wextra -Werror -I../../main \
    test_sl2_pair_v5.c ../../main/sl2_link.c -lm -o /tmp/test_sl2_pair_v5
/tmp/test_sl2_pair_v5
