#!/usr/bin/env bash
# Bond a Dial to a unit over two serial ports. Reads each MAC, generates a
# random per-pair LMK, and writes the bond to both via their console commands.
#
# Usage: ./scripts/pair_dial.sh <unit_port> <dial_port>
# Example: ./scripts/pair_dial.sh /dev/ttyACM0 /dev/ttyACM1
#
# Requirements:
#   - python3 + pyserial  (pip install pyserial)
#   - openssl             (for random LMK generation)
#   - Both devices flashed and connected over USB
#   - PMK: exported PMK_HEX=<32 hex> or PMK_FILE (default ~/.espnow_test_pmk)
#
# Both devices restart after pairing. The Dial's offline indicator should
# clear within ~5 s once they reboot and lock channels.
set -euo pipefail

UNIT_PORT="${1:?unit serial port (e.g. /dev/ttyACM0)}"
DIAL_PORT="${2:?dial serial port (e.g. /dev/ttyACM1)}"
PY="${PYTHON:-python3}"

# Optional PMK provisioning. Both ends must share the same 16-byte PMK for the
# encrypted link to authenticate; since 68747a7 the unit reads it from NVS, so
# a bond written without a PMK yields a unit that says "bonded" but never links.
# PMK_HEX=<32 hex> wins; else PMK_FILE (default ~/.espnow_test_pmk) is read as
# 16 raw bytes and hex-encoded. If neither is available we warn and skip.
PMK_HEX="${PMK_HEX:-}"
PMK_FILE="${PMK_FILE:-$HOME/.espnow_test_pmk}"
if [ -z "$PMK_HEX" ] && [ -f "$PMK_FILE" ]; then
  PMK_HEX="$(head -c 16 "$PMK_FILE" | od -An -v -tx1 | tr -d ' \n')"
fi
if [ -n "$PMK_HEX" ] && [ "${#PMK_HEX}" -ne 32 ]; then
  echo "ERROR: PMK must be exactly 32 hex chars (16 bytes), got ${#PMK_HEX}"; exit 1
fi

read_mac() {  # $1 = port
  "$PY" - "$1" <<'EOF'
import sys, time, serial
port = sys.argv[1]
s = serial.Serial(port, 115200, timeout=2)
time.sleep(0.3); s.reset_input_buffer()
s.write(b"espnow-mac\r\n"); time.sleep(0.5)
out = s.read(4096).decode(errors="ignore")
for line in out.splitlines():
    if "ESPNOW-MAC" in line:
        print(line.split()[-1]); break
else:
    print(f"no MAC from {port}", file=sys.stderr)
    sys.exit(1)
s.close()
EOF
}

send_pair() {  # $1 = port, $2 = peer mac, $3 = lmk hex
  "$PY" - "$1" "$2" "$3" <<'EOF'
import sys, time, serial
port, mac, lmk = sys.argv[1], sys.argv[2], sys.argv[3]
s = serial.Serial(port, 115200, timeout=2)
time.sleep(0.3); s.reset_input_buffer()
s.write(f"espnow-pair {mac} {lmk}\r\n".encode()); time.sleep(0.8)
out = s.read(4096).decode(errors="ignore").strip()
print(out)
s.close()
if "ESPNOW-PAIR OK" not in out:
    sys.exit(1)
EOF
}

send_pmk() {  # $1 = port, $2 = pmk hex; nonzero exit if not acknowledged
  "$PY" - "$1" "$2" <<'EOF'
import sys, time, serial
port, pmk = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=2)
time.sleep(0.3); s.reset_input_buffer()
s.write(f"espnow-pmk {pmk}\r\n".encode()); time.sleep(0.8)
out = s.read(4096).decode(errors="ignore").strip()
print(out)
s.close()
if "ESPNOW-PMK OK" not in out:
    sys.exit(1)
EOF
}

echo "Reading MACs..."
UNIT_MAC="$(read_mac "$UNIT_PORT")"
DIAL_MAC="$(read_mac "$DIAL_PORT")"
echo "unit=$UNIT_MAC dial=$DIAL_MAC"
[ -n "$UNIT_MAC" ] && [ -n "$DIAL_MAC" ] || { echo "ERROR: failed to read a MAC"; exit 1; }

if [ -n "$PMK_HEX" ]; then
  echo "-- provisioning unit PMK --"
  send_pmk "$UNIT_PORT" "$PMK_HEX" \
    || { echo "ERROR: unit did not ack espnow-pmk (unit firmware must be >= 0.2.5 with the espnow-pmk command)"; exit 1; }
  echo "-- provisioning dial PMK --"
  send_pmk "$DIAL_PORT" "$PMK_HEX" \
    || echo "WARN: dial did not ack espnow-pmk (older dial fw bakes its PMK at build) — ensure it matches"
else
  echo "WARN: no PMK provided (set PMK_HEX or PMK_FILE) — the unit must already have a PMK in NVS or the link will not authenticate"
fi

LMK="$(openssl rand -hex 16)"
echo "lmk=$LMK"
echo "-- pairing unit --"; send_pair "$UNIT_PORT" "$DIAL_MAC" "$LMK"
echo "-- pairing dial --"; send_pair "$DIAL_PORT" "$UNIT_MAC" "$LMK"
echo "done; both devices restart and should link within a few seconds."
