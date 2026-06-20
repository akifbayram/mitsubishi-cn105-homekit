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
#
# Both devices restart after pairing. The Dial's offline indicator should
# clear within ~5 s once they reboot and lock channels.
set -euo pipefail

UNIT_PORT="${1:?unit serial port (e.g. /dev/ttyACM0)}"
DIAL_PORT="${2:?dial serial port (e.g. /dev/ttyACM1)}"
PY="${PYTHON:-python3}"

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

echo "Reading MACs..."
UNIT_MAC="$(read_mac "$UNIT_PORT")"
DIAL_MAC="$(read_mac "$DIAL_PORT")"
echo "unit=$UNIT_MAC dial=$DIAL_MAC"
[ -n "$UNIT_MAC" ] && [ -n "$DIAL_MAC" ] || { echo "ERROR: failed to read a MAC"; exit 1; }

LMK="$(openssl rand -hex 16)"
echo "lmk=$LMK"
echo "-- pairing unit --"; send_pair "$UNIT_PORT" "$DIAL_MAC" "$LMK"
echo "-- pairing dial --"; send_pair "$DIAL_PORT" "$UNIT_MAC" "$LMK"
echo "done; both devices restart and should link within a few seconds."
