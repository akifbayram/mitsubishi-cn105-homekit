#!/usr/bin/env python3
"""Compile current sensor modules against clock, CN105, and NimBLE boundaries."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"


def clean(name):
    return re.sub(r"^#(?:include[^\n]*|pragma once)\n", "", (MAIN / name).read_text(), flags=re.M)


def module(name, prefix):
    # These file-local names overlap when the three production modules share
    # one host translation unit. Their bodies otherwise compile unchanged.
    return re.sub(r"\b(TAG|s_mux|s_pendingClear|s_feed)\b", prefix + r"_\1", clean(name))


source = (HERE / "host.h").read_text()
source += clean("esp_utils.h").split("// Human-readable")[0]
source += clean("ble_sensor.h") + clean("link_sensor.h") + clean("room_avg.h")
source += module("ble_sensor.cpp", "ble")
source += module("link_sensor.cpp", "link") + module("room_avg.cpp", "avg")
source += (HERE / "test_recovery.cpp").read_text()
cases = re.findall(r'CASE\("([^"]+)"\)', (HERE / "test_recovery.cpp").read_text())
failed = False
with tempfile.TemporaryDirectory(prefix="cn105-sensor-recovery-") as directory:
    build = Path(directory)
    path = build / "recovery.cpp"
    path.write_text(source)
    executable = build / "recovery"
    subprocess.run(["g++", "-std=c++17", "-DBLE_ENABLE", "-I" + str(MAIN), str(path),
                    "-o", str(executable)], check=True)
    for case in cases:
        if len(sys.argv) > 1 and case not in sys.argv[1:]:
            continue
        result = subprocess.run([str(executable), case])
        print(f"sensor_recovery/{case}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
        failed |= result.returncode != 0
sys.exit(1 if failed else 0)
