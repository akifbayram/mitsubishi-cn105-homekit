#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
python3 -B test_web_startup.py
