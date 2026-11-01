#!/usr/bin/env python3
"""Exercise sensor identity across callback interleaving and actual Link packets."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"


def function(source, name):
    match = re.search(r"^(?:(?:static|inline) )*(?:bool|void|uint8_t|uint32_t|int) "
                      + re.escape(name) + r"\(", source, re.M)
    assert match, name
    opening = source.index("{", match.start())
    level, end = 1, opening + 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def clean(name):
    return re.sub(r"^#(?:include[^\n]*|pragma once)\n", "", (MAIN / name).read_text(), flags=re.M)


with tempfile.TemporaryDirectory(prefix="cn105-sensor-identity-") as directory:
    build = Path(directory)
    (build / "clock.inc").write_text(clean("esp_utils.h").split("// Human-readable")[0])
    ble = (MAIN / "ble_sensor.cpp").read_text()
    (build / "ble_state.inc").write_text(ble[ble.index("struct SlotState"):ble.index("// ── Discovery state")])
    (build / "ble_functions.inc").write_text("\n".join(function(ble, name) for name in
        ["BleSensor::feedSlot", "slotIdxOk", "BleSensor::setSensor", "gap_event_cb"]))
    (build / "link_sensor.inc").write_text(clean("link_sensor.h") + clean("link_sensor.cpp"))
    adapter = (MAIN / "espnow_link.cpp").read_text()
    (build / "room_adapter.inc").write_text(function((MAIN / "room_avg.cpp").read_text(), "RoomAvg::memberAvailable") + "\n" +
        function((MAIN / "esp_utils.h").read_text(), "fnv1a32") + "\n" +
        "\n".join(function(adapter, name) for name in
            ["copy_name", "room_catalog_build", "room_catalog_hash", "room_catalog_revision", "h_room_source_set"]))
    core = build / "sl2_link.o"
    subprocess.run(["gcc", "-std=c11", "-I" + str(MAIN), "-c", str(MAIN / "sl2_link.c"), "-o", str(core)], check=True)
    failed = False
    for name in ["ble", "link"]:
        if len(sys.argv) > 1 and name not in sys.argv[1:]:
            continue
        source = HERE / ("test_" + name + ".cpp")
        exe = build / name
        subprocess.run(["g++", "-std=c++17", "-I" + str(MAIN), "-I" + str(build), str(source),
                        *([str(core)] if name == "link" else []), "-o", str(exe)], check=True)
        for case in re.findall(r'CASE\("([^"]+)"\)', source.read_text()):
            result = subprocess.run([str(exe), case])
            print(f"sensor_identity/{name}/{case}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
            failed |= result.returncode != 0
    sys.exit(1 if failed else 0)
