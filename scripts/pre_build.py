Import("env")

import subprocess
import sys
import os
import json

# Skip during IDE integration dumps (IntelliSense, etc.)
if env.IsIntegrationDump():
    # noinspection PyUnresolvedReferences
    Return()

project_dir = env.subst("$PROJECT_DIR")
embed_script = os.path.join(project_dir, "scripts", "embed_html.py")

# Defaults for template substitution (matches branding.h)
brand_vars = {
    "BRAND_NAME": "Mini Split",
    "BRAND_AP_PREFIX": "AC",
    "BRAND_AP_PASSWORD": "homekit1",
    "BRAND_MANUFACTURER": "Mitsubishi Electric",
    "BRAND_MODEL": "CN105",
    "BRAND_QR_ID": "MCAC",
    "BRAND_THEME_COLOR": "#f48120",
}
# Override with any -DBRAND_* build flags
for flag in env.get("BUILD_FLAGS", []):
    flag_str = str(flag)
    if flag_str.startswith("-DBRAND_") and "=" in flag_str:
        key, val = flag_str[2:].split("=", 1)
        brand_vars[key] = val.strip("'\"")

brand_json = json.dumps(brand_vars)

# List of (source HTML, output header) pairs
html_files = [
    (os.path.join(project_dir, "web", "index.html"),
     os.path.join(project_dir, "include", "web_ui_html.h")),
    (os.path.join(project_dir, "web", "recovery.html"),
     os.path.join(project_dir, "include", "wifi_recovery_html.h")),
]

for html_src, html_hdr in html_files:
    if not os.path.isfile(html_src):
        continue
    # Skip if header is newer than source AND brand vars haven't changed
    brand_hash_file = html_hdr + ".brand"
    prev_brand = ""
    if os.path.isfile(brand_hash_file):
        with open(brand_hash_file) as f:
            prev_brand = f.read()
    if (os.path.isfile(html_hdr) and os.path.getmtime(html_hdr) >= os.path.getmtime(html_src)
            and prev_brand == brand_json):
        continue

    name = os.path.basename(html_src)
    print(f"Pre-build: Regenerating {os.path.basename(html_hdr)} from {name}...")
    result = subprocess.run(
        [sys.executable, embed_script, html_src, html_hdr, brand_json],
        capture_output=True, text=True, cwd=project_dir
    )
    if result.stdout:
        print(result.stdout.strip())
    if result.returncode != 0:
        print(f"ERROR: embed_html.py failed for {name}:\n{result.stderr}", file=sys.stderr)
        env.Exit(1)
    # Remember which brand vars were used so we regenerate on env switch
    with open(brand_hash_file, "w") as f:
        f.write(brand_json)

# ── Icon embedding (PNG → PROGMEM, no gzip) ─────────────────────────────
embed_binary_script = os.path.join(project_dir, "scripts", "embed_binary.py")

icon_files = [
    (os.path.join(project_dir, "web", "icon-192.png"),
     os.path.join(project_dir, "include", "icon_192_png.h")),
    (os.path.join(project_dir, "web", "icon-512.png"),
     os.path.join(project_dir, "include", "icon_512_png.h")),
]

for icon_src, icon_hdr in icon_files:
    if not os.path.isfile(icon_src):
        continue
    # Skip if header is newer than source
    if (os.path.isfile(icon_hdr)
            and os.path.getmtime(icon_hdr) >= os.path.getmtime(icon_src)):
        continue
    name = os.path.basename(icon_src)
    print(f"Pre-build: Embedding {name}...")
    result = subprocess.run(
        [sys.executable, embed_binary_script, icon_src, icon_hdr],
        capture_output=True, text=True, cwd=project_dir
    )
    if result.stdout:
        print(result.stdout.strip())
    if result.returncode != 0:
        print(f"ERROR: embed_binary.py failed for {name}:\n{result.stderr}", file=sys.stderr)
        env.Exit(1)
