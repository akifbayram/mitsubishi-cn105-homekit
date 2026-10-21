#!/usr/bin/env bash
# The Firmware card's version menu turns the release index (versions.json) into
# pinned rows, "Other versions" and the one update to offer. That logic is pure
# and lives in web/index.html between the ver-menu markers; test.js pins it.
# It leans on the ver-compare block, so both are evaluated together.
set -euo pipefail
cd "$(dirname "$0")"
node ./test.js
