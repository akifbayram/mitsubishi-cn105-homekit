#!/usr/bin/env bash
# Keep shell syntax out of esp-idf-ci-action's single-quoted command input.
set -euo pipefail

target=${1:?target is required}
board=${2:?board is required}
case "$target/$board" in
  esp32c6/nanoc6) profile=NANOC6 ;;
  esp32s3/m5atoms3-lite) profile=M5ATOMS3_LITE ;;
  *) echo "Unsupported release target/board: $target/$board" >&2; exit 1 ;;
esac

# Exercise version derivation with the Git installed in the build container.
bash test/project_ver/run.sh

# Tag builds record this before entering the container. Manual branch builds
# have no tag file and retain project_ver.py's normal git-describe behavior.
tag=$(cat .release-tag 2>/dev/null || true)
idf.py set-target "$target"
idf.py "-DRELEASE_TAG=$tag" "-DBOARD_PROFILE_${profile}=1" build
