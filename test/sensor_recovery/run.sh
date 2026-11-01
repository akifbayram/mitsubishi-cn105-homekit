#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
python3 test_sensor_recovery.py "$@"
