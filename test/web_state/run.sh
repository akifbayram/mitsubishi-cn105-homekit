#!/usr/bin/env bash
# Two things the browser gets wrong silently, so neither is left to review.
#
# 1. Unit switching. Re-entering the °F branch for sliders that already show °F
#    ran every handle through the C→F table twice — 72 °F read as 72 °C, whose
#    nearest row is 30.5 °C = 88 °F — and the slider then sent 30.5 °C to the
#    heat pump. Both branches must be idempotent, on all three sliders.
#
# 2. Partial state frames. pushState() rolls back whole sections rather than
#    dropping an oversized frame (web_ws.cpp), so a field can be MISSING rather
#    than zero. toggleRoomMember() XORs against roomMembers and persists the
#    result, so absent must not read as 0 and clear every other member.
set -euo pipefail
cd "$(dirname "$0")"
node ./test.js
