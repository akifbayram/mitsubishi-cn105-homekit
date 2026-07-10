#!/usr/bin/env bash
# Pair a Dial to a unit over two serial ports, using Serin Link v2's
# over-the-air signed pairing: open the unit's 60 s pairing window, kick the
# dial's 'pair' verb, then poll the unit until the bond commits.
#
# v2 replaced the v1 serial bond injection (espnow-pmk / espnow-pair <mac>
# <lmk>): the per-pair key is derived inside the X25519+Ed25519 handshake and
# nothing secret crosses the serial ports — this script is just automation
# around the two console verbs a human would otherwise type.
#
# Usage: ./scripts/pair_dial.sh <unit_port> <dial_port>
# Example: ./scripts/pair_dial.sh /dev/ttyACM0 /dev/ttyACM1
#
# Requirements:
#   - python3 + pyserial  (pip install pyserial)
#   - Both devices flashed with Serin Link v2 firmware and connected over USB
#
# On success the dial reboots itself and re-locks onto the unit within ~5 s.
set -euo pipefail

UNIT_PORT="${1:?unit serial port (e.g. /dev/ttyACM0)}"
DIAL_PORT="${2:?dial serial port (e.g. /dev/ttyACM1)}"
PY="${PYTHON:-python3}"

console() {  # $1 = port, $2 = command; echoes ~1.5 s of device output
  "$PY" - "$1" "$2" <<'EOF'
import sys, time, serial
port, cmd = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=0.2)
s.write((cmd + "\r\n").encode()); s.flush()
end = time.time() + 1.5
out = b""
while time.time() < end:
    out += s.read(256)
s.close()
sys.stdout.write(out.decode(errors="replace"))
EOF
}

echo "== opening the unit's pairing window (60 s) =="
console "$UNIT_PORT" "espnow-pair" | grep -q "ESPNOW-PAIR" \
  || { echo "ERROR: unit did not ack espnow-pair (needs Serin Link v2 firmware)"; exit 1; }

echo "== starting the dial's signed pairing sweep =="
console "$DIAL_PORT" "pair" > /dev/null \
  || { echo "ERROR: could not reach the dial console on $DIAL_PORT"; exit 1; }

echo "== waiting for the bond to commit (channel sweep can take ~10-45 s) =="
for _ in $(seq 1 24); do
  sleep 3
  st="$(console "$UNIT_PORT" "espnow-status" | grep -o 'ESPNOW-STATUS.*' | tail -1 | tr -d '\r')"
  echo "  ${st:-<no status>}"
  case "$st" in
    *result=paired*)
      echo "PAIRED OK — the dial reboots and re-locks within ~5 s."
      exit 0 ;;
    *result=timeout*|*result=full*|*result=pin-mismatch*)
      echo "ERROR: pairing ended without a bond: $st"
      echo "       (pin-mismatch: the dial knows a different identity for this"
      echo "        unit — forget the zone on the dial first if intentional)"
      exit 1 ;;
  esac
done
echo "ERROR: no bond after ~72 s. Watch both consoles (idf.py monitor) and retry."
exit 1
