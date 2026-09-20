#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
python3 extract.py ../../main/web_ws.cpp "$build_dir/boundary.inc"
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Istubs -I../../main -I"$build_dir" test_boundary.cpp -o "$build_dir/test_boundary"
"$build_dir/test_boundary"
