#!/usr/bin/env bash
# web/*.html ship as one gzipped blob with their JS inline, and nothing in the
# firmware build parses that JS. A syntax error therefore builds, flashes and
# boots clean, and only shows up as a web UI that renders and then does
# nothing — no console access on a headless board to find out why.
#
# Parse every inline <script> with node so a typo fails the build instead.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import os
import re
import subprocess
import sys
import tempfile

fail = 0
for path in sorted(f for f in os.listdir('web') if f.endswith('.html')):
    src = open(os.path.join('web', path), encoding='utf-8').read()
    # Skip <script src=...> — only inline bodies have anything to parse.
    blocks = re.findall(r'<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>', src, re.S)
    for i, body in enumerate(blocks):
        if not body.strip():
            continue
        with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False,
                                         encoding='utf-8') as f:
            f.write(body)
            tmp = f.name
        try:
            r = subprocess.run(['node', '--check', tmp],
                               capture_output=True, text=True)
        finally:
            os.unlink(tmp)
        if r.returncode:
            fail = 1
            print(f"FAIL: web/{path} script block {i} does not parse:")
            for line in r.stderr.splitlines()[:12]:
                print(f"    {line}")

if not fail:
    print("PASS: web/*.html inline scripts parse")
sys.exit(fail)
PY
