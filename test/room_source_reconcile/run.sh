#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
python3 extract.py ../../main/espnow_link.cpp "$build_dir/room_source.inc" \
    copy_name room_catalog_build room_source_reconcile_catalog
g++ -std=c++17 -Wall -Wextra -Werror -Istubs -I../../main -I"$build_dir" \
    test_room_source_reconcile.cpp -lm -o "$build_dir/test_room_source_reconcile"
"$build_dir/test_room_source_reconcile"
