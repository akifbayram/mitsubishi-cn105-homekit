#!/usr/bin/env bash
# The device's update check orders firmware versions with cmpVer(). Once a beta
# channel exists that ordering decides whether a prerelease is offered, whether
# a stable release supersedes it, and whether "return to stable" is a downgrade.
#
# The vector table in test.js is the contract, and it is duplicated verbatim in
# serin-labs.github.io/tests/version-compare.test.js. Change one, change both.
set -euo pipefail
cd "$(dirname "$0")"
node ./test.js
