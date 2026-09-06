#!/usr/bin/env bash
# scripts/project_ver.py decides the version string baked into every shipped
# binary, and nothing used to test it. Three releases went out labelled "-dirty"
# because the check could not see past idf-component-manager's target: stamp in
# dependencies.lock, and the first attempt to fix that traded the false dirty
# for a false CLEAN — excluding the lock by path would have hidden a real change
# to the resolved component set, which is the one thing the committed lock
# exists to prevent (see .gitignore and main/idf_component.yml).
#
# So both directions are pinned here. The device serves this string to the
# browser updater, which orders it with cmpVer(); a wrong one either strands a
# unit on "up to date" forever or nags a fully-updated one. The vectors below
# are the contract — every case names the failure it prevents.
#
# Fixtures are seeded from the real dependencies.lock so the target-stamp rule
# is exercised against the file it was written for, not a synthetic stand-in.
set -euo pipefail
cd "$(dirname "$0")"

SCRIPT="$PWD/../../scripts/project_ver.py"
REALLOCK="$PWD/../../dependencies.lock"
CMAKELISTS="$PWD/../../CMakeLists.txt"

# Hermetic: no global config, and an identity of our own. build.yml's runner has
# no git identity configured (firmware-release.yml sets one by hand before its
# commit), and a developer's ~/.gitconfig could otherwise inject core.autocrlf,
# commit.gpgsign or hooks into these fixtures.
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid
export GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
export GIT_AUTHOR_DATE="2026-01-01T00:00:00Z"
export GIT_COMMITTER_DATE="2026-01-01T00:00:00Z"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0

# A clean repo tagged v0.2.5, carrying the real lock.
mk() {
  local d="$TMP/$1"
  mkdir -p "$d/main"
  git init -q -b main "$d"
  cp "$REALLOCK" "$d/dependencies.lock"
  echo 'int main(){}' > "$d/main/main.cpp"
  git -C "$d" add -A
  git -C "$d" commit -qm init
  git -C "$d" tag -a v0.2.5 -m release
  echo "$d"
}

report() { # report <0 for pass, anything else for fail> <what it pins>
  if [ "$1" = 0 ]; then
    pass=$((pass + 1)); printf '  ok   %s\n' "$2"
  else
    fail=$((fail + 1)); printf '  FAIL %s\n' "$2"
  fi
}

ck() { # ck <what it pins> <dir> <expected version> [release tag]
  local got
  local -a args=(--repo "$2")
  # Four arguments means "pass --release-tag", tested with $# rather than a
  # non-empty $4: one vector below deliberately passes an EMPTY tag to prove an
  # empty override still falls back to git describe, and an emptiness test would
  # silently reroute it to the no-override path and stop testing what it names.
  if [ "$#" -ge 4 ]; then args+=("--release-tag=$4"); fi
  got="$(python3 "$SCRIPT" "${args[@]}" 2>/dev/null)" || got="<script failed>"
  if [ "$got" = "$3" ]; then
    report 0 "$(printf '%-44s -> %s' "$1" "$got")"
  else
    report 1 "$(printf '%-44s -> %s (want %s)' "$1" "$got" "$3")"
  fi
}

# Structural assertions over the files the tolerance and the seam depend on.
# Named by polarity so the meaning is in the call, not in a bare 0/1 one line
# away from the description.
have() { # have <what it pins> <extended regex> <file...>
  local what="$1" pat="$2"; shift 2
  if grep -qE "$pat" "$@"; then report 0 "$what"; else report 1 "$what"; fi
}

lack() { # lack <what it pins> <extended regex> <file...>
  local what="$1" pat="$2"; shift 2
  if grep -qE "$pat" "$@"; then report 1 "$what"; else report 0 "$what"; fi
}

echo "=== version derivation ==="

d=$(mk clean);            ck "clean tagged tree"                    "$d" "v0.2.5"

# git diff-index trusts the index stat cache and never refreshes it, so a
# checkout that only skewed an mtime reported a dirty that git describe --dirty
# never did. A distinctly different mtime is required — a plain touch lands
# inside the racy-git window and hides the bug.
d=$(mk mtime); touch -d '2030-01-01' "$d/main/main.cpp"
                          ck "stat-only touch, identical bytes"     "$d" "v0.2.5"

d=$(mk edited); echo '// edit' >> "$d/main/main.cpp"
                          ck "real source edit"                     "$d" "v0.2.5-dirty"

d=$(mk untracked); echo junk > "$d/untracked.txt"
                          ck "untracked file ignored"               "$d" "v0.2.5"

# git describe --always fell back to a bare SHA, which parseVer() reads as a
# version number: '39942af' parses as core [39942], above every release that
# will ever ship. build.yml checks out at the default fetch-depth and fetches
# no tags, so that fallback was its normal case.
d=$(mk notags); git -C "$d" tag -d v0.2.5 >/dev/null
                          ck "no release tag reachable"             "$d" "0.0.0-dev"

# Without --match any reachable tag becomes the firmware version.
d=$(mk othertag); git -C "$d" tag -a bench-2026-08-29 -m bench
                          ck "non-release tag ignored"              "$d" "v0.2.5"

d="$TMP/norepo"; mkdir -p "$d"
                          ck "not a git repository"                 "$d" "0.0.0-dev"

echo "=== dependencies.lock: the benign stamp, and nothing else ==="

# The whole reason this code is special-cased: CI builds esp32c6 and esp32s3
# from one tagged checkout and the committed lock records esp32c6, so the
# esp32s3 job always leaves the stamp flipped.
d=$(mk stamp); sed -i 's/^target: .*/target: esp32s3/' "$d/dependencies.lock"
                          ck "target stamp flipped (the bug)"       "$d" "v0.2.5"

# mdns is bounded only as ~1.11.3, so a registry patch release rewrites the
# resolved set. Excluding the lock by path would have called this clean.
d=$(mk mdns); sed -i 's/version: 1.11.3/version: 1.11.4/' "$d/dependencies.lock"
                          ck "resolved component version changed"   "$d" "v0.2.5-dirty"

d=$(mk chash); sed -i '0,/component_hash: ./s/component_hash: ./component_hash: 0/' "$d/dependencies.lock"
                          ck "component_hash changed"               "$d" "v0.2.5-dirty"

d=$(mk mhash); sed -i 's/^manifest_hash: ./manifest_hash: 0/' "$d/dependencies.lock"
                          ck "manifest_hash changed"                "$d" "v0.2.5-dirty"

# direct_dependencies is a YAML sequence, so dropping an item produces the patch
# line "-- espressif/mdns". A filter that skips lines starting with "-" as diff
# headers would read that as clean; git does the comparison, so it cannot.
d=$(mk dropdep); sed -i '/^- espressif\/mdns$/d' "$d/dependencies.lock"
                          ck "direct dependency removed"            "$d" "v0.2.5-dirty"

d=$(mk both); sed -i 's/^target: .*/target: esp32s3/; s/version: 1.11.3/version: 1.11.4/' "$d/dependencies.lock"
                          ck "stamp plus a real change"             "$d" "v0.2.5-dirty"

d=$(mk crlf); sed -i 's/$/\r/' "$d/dependencies.lock"
                          ck "line endings rewritten"               "$d" "v0.2.5-dirty"

d=$(mk trailnl); printf '\n' >> "$d/dependencies.lock"
                          ck "trailing newline added"               "$d" "v0.2.5-dirty"

# -I is hunk-shaped, not positional, so it would forgive an inserted second
# stamp on its own. The line-count guard is what refuses it.
d=$(mk twostamps); printf 'target: esp32s3\n' >> "$d/dependencies.lock"
                          ck "second target line inserted"          "$d" "v0.2.5-dirty"

d=$(mk nolock); rm "$d/dependencies.lock"
                          ck "lock deleted from the worktree"       "$d" "v0.2.5-dirty"

echo "=== the release path: firmware-release.yml hands over the pushed tag ==="

# The tag is used verbatim, so the binary and the manifest published from the
# same github.ref_name cannot disagree — even here, where describe would pick
# the other tag by tagger date.
d=$(mk reltag); GIT_COMMITTER_DATE="2026-02-02T00:00:00Z" git -C "$d" tag -a v0.2.5-beta.3 -m beta
                          ck "two tags, no override: describe guesses"  "$d" "v0.2.5-beta.3"
                          ck "two tags, override wins"              "$d" "v0.2.5" "v0.2.5"

# An empty override is the normal case everywhere but a tag push, and must fall
# straight back to git describe rather than produce an empty version.
d=$(mk emptytag);         ck "empty override falls back to describe" "$d" "v0.2.5" ""

# The dirty checks still run on the release path. Taking the tag as proof of
# cleanliness would reopen the exact hole this script exists to close: a release
# binary is the one people install, so an unexpected lock change matters most there.
d=$(mk reldirty); sed -i 's/version: 1.11.3/version: 1.11.4/' "$d/dependencies.lock"
                          ck "override + real lock change"          "$d" "v0.2.6-dirty" "v0.2.6"

d=$(mk relstamp); sed -i 's/^target: .*/target: esp32s3/' "$d/dependencies.lock"
                          ck "override + benign stamp only"         "$d" "v0.2.6" "v0.2.6"

# A tagless checkout still yields a real version on the release path — the
# override does not need describe to succeed.
d=$(mk reltagless); git -C "$d" tag -d v0.2.5 >/dev/null
                          ck "override with no tags reachable"      "$d" "v0.2.6" "v0.2.6"

echo "=== vendored under a larger repo ==="

# Both pathspecs are relative to the project directory. Anchoring them at the
# repository root instead would scope the check to the wrong tree once this is
# vendored, which the integration-variants repo and the Matter fork both do.
v="$TMP/outer"; mkdir -p "$v/fw/main"
git init -q -b main "$v"
cp "$REALLOCK" "$v/fw/dependencies.lock"
echo 'int main(){}' > "$v/fw/main/main.cpp"
echo outer > "$v/outer.txt"
git -C "$v" add -A; git -C "$v" commit -qm init; git -C "$v" tag -a v0.2.5 -m release
echo 'modified' >> "$v/outer.txt"
                          ck "outer repo dirty, project clean"      "$v/fw" "v0.2.5"
sed -i 's/^target: .*/target: esp32s3/' "$v/fw/dependencies.lock"
                          ck "vendored: own stamp flipped"          "$v/fw" "v0.2.5"
echo '// edit' >> "$v/fw/main/main.cpp"
                          ck "vendored: own source edited"          "$v/fw" "v0.2.5-dirty"

echo "=== assumptions the target-stamp tolerance rests on ==="

# Verified against idf-component-manager 2.5.0: with only IDF_TARGET changed,
# target: is the sole differing line. That holds because nothing in this
# closure is target-conditional. If any of these three stops being true, a
# target flip can legitimately change the resolved set and the tolerance would
# be forgiving a real difference — so fail here rather than ship a clean-looking
# binary built from an untested component set.
# if/then, not `[ ... ]; report $?` — a bare failing test under `set -e` aborts
# the script instead of reporting, which would skip every assertion below it.
n=$(grep -cE '^target: [a-z0-9]+$' "$REALLOCK" || true)
if [ "$n" = 1 ]; then r=0; else r=1; fi
report "$r" "dependencies.lock has exactly one target: line (found $n)"

lack "no component declares targets: (would force a re-solve)" \
     '^[[:space:]]+targets:' "$REALLOCK"

lack "no rule in main/idf_component.yml keys on target" \
     'if:.*\btarget\b' "$PWD/../../main/idf_component.yml"

# The submodule half has to be guarded, not globbed: build.yml's host-tests
# checkout has no submodules:, so components/esp-homekit-sdk is empty there and
# an unguarded glob would expand to a literal path, grep would exit 2, and the
# assertion would pass vacuously — reporting green for a file it never read.
SDK_MANIFESTS="$PWD/../../components/esp-homekit-sdk/components/homekit"
if [ -f "$SDK_MANIFESTS/esp_hap_core/idf_component.yml" ]; then
  lack "no esp-homekit-sdk manifest rule keys on target" \
       'if:.*\btarget\b' "$SDK_MANIFESTS"/*/idf_component.yml
else
  printf '  skip esp-homekit-sdk manifests (submodule not checked out)\n'
fi

# The seam itself: if CMakeLists stops calling the script, or goes back to
# inline git, everything above silently stops covering the shipped binary.
have "CMakeLists.txt still derives PROJECT_VER from the script" \
     'scripts/project_ver\.py' "$CMAKELISTS"

lack "CMakeLists.txt does not call git inline again" \
     '^[[:space:]]*COMMAND git ' "$CMAKELISTS"

# The release half of the seam. Without it the build re-derives its own version
# with git describe, which tie-breaks two tags on one commit by tagger date
# while the manifest comes from github.ref_name — and nothing above would fail.
WORKFLOW="$PWD/../../.github/workflows/firmware-release.yml"
have "firmware-release.yml passes the pushed tag to the build" \
     'DRELEASE_TAG=' "$WORKFLOW"

have "CMakeLists.txt forwards RELEASE_TAG to the script" \
     'release-tag' "$CMAKELISTS"

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
