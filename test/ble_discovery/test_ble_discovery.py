#!/usr/bin/env python3
"""Run the real BLE scan-duty and discovery-session code on the host.

The functions are extracted unchanged from main/ble_sensor.cpp; only NimBLE,
the CN105 link and settings persistence are stubbed (test_discovery.cpp).
Each CASE runs in its own process so state never leaks between cases.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"

FUNCTIONS = [
    "BleSensor::feedSlot", "freshAt", "targetConfigured", "wantScan",
    "startScan", "stopScan", "drainPendingClear", "BleSensor::loop",
    "BleSensor::setBleEnabled", "BleSensor::startDiscovery",
    "BleSensor::stopDiscovery", "BleSensor::isDiscovering",
    "BleSensor::discoveryElapsedMs", "BleSensor::pollDiscoveryComplete",
]


def function(source, name):
    match = re.search(r"^(?:(?:static|inline) )*(?:bool|void|uint32_t|int) "
                      + re.escape(name) + r"\([^;{]*\{", source, re.M)   # a definition, not a prototype
    if not match:
        return None
    opening = source.index("{", match.start())
    level, end = 1, opening + 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


with tempfile.TemporaryDirectory(prefix="cn105-ble-discovery-") as directory:
    build = Path(directory)
    ble = (MAIN / "ble_sensor.cpp").read_text()
    header = (MAIN / "ble_sensor.h").read_text()
    constants = "\n".join(l for l in header.splitlines() if l.startswith("constexpr "))
    device = header[header.index("struct BleDiscoveredDevice"):header.index("\nnamespace BleSensor")]
    state = ble[ble.index("struct SlotState"):ble.index("static bool targetConfigured")]
    # A function the code under test does not have yet is left out, so the
    # test fails to compile against it: that is the expected red for new API.
    bodies = [b for b in (function(ble, n) for n in FUNCTIONS) if b]
    (build / "ble_header.inc").write_text(constants + "\n" + device + "\n")
    (build / "ble_state.inc").write_text(state)
    (build / "ble_functions.inc").write_text("\n".join(bodies))
    source = HERE / "test_discovery.cpp"
    exe = build / "discovery"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-I" + str(build),
                    str(source), "-o", str(exe)], check=True)
    failed = False
    for case in re.findall(r'CASE\("([^"]+)"\)', source.read_text()):
        if len(sys.argv) > 1 and case not in sys.argv[1:]:
            continue
        result = subprocess.run([str(exe), case])
        print(f"ble_discovery/{case}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
        failed |= result.returncode != 0
    sys.exit(1 if failed else 0)
