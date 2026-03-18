#!/usr/bin/env python3
"""Gzip an HTML file with {{BRAND_*}} template substitution for ESP-IDF CMake builds.

Usage:
    python3 embed_html_idf.py --input <html_file> --output <gz_file> \
        [--brand-name X] [--brand-ap-prefix X] [--brand-ap-password X] \
        [--brand-model X] [--brand-theme-color X]
"""

import argparse
import gzip
import os
import sys


# Defaults matching branding.h
DEFAULTS = {
    "BRAND_NAME": "Serin",
    "BRAND_AP_PREFIX": "Serin",
    "BRAND_AP_PASSWORD": "serinlabs",
    "BRAND_MODEL": "CN105",
    "BRAND_THEME_COLOR": "#1a73e8",
}


def embed(input_file: str, output_file: str, brand_vars: dict) -> None:
    if not os.path.isfile(input_file):
        print(f"ERROR: {input_file} not found", file=sys.stderr)
        sys.exit(1)

    with open(input_file, "rb") as f:
        raw = f.read()

    # Template substitution: replace {{KEY}} with value
    text = raw.decode("utf-8")
    for key, val in brand_vars.items():
        text = text.replace("{{" + key + "}}", val)
    raw = text.encode("utf-8")

    compressed = gzip.compress(raw, compresslevel=9)
    raw_size = len(raw)
    gz_size = len(compressed)

    os.makedirs(os.path.dirname(os.path.abspath(output_file)), exist_ok=True)
    with open(output_file, "wb") as f:
        f.write(compressed)

    print(f"Embedded {input_file}")
    print(f"  Raw size:    {raw_size:,} bytes")
    print(f"  Gzipped:     {gz_size:,} bytes")
    print(f"  Ratio:       {gz_size / raw_size * 100:.1f}%")
    print(f"  Output:      {output_file}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Gzip HTML with brand template substitution")
    parser.add_argument("--input", required=True, help="Input HTML file")
    parser.add_argument("--output", required=True, help="Output .gz file")
    parser.add_argument("--brand-name", default=DEFAULTS["BRAND_NAME"])
    parser.add_argument("--brand-ap-prefix", default=DEFAULTS["BRAND_AP_PREFIX"])
    parser.add_argument("--brand-ap-password", default=DEFAULTS["BRAND_AP_PASSWORD"])
    parser.add_argument("--brand-model", default=DEFAULTS["BRAND_MODEL"])
    parser.add_argument("--brand-theme-color", default=DEFAULTS["BRAND_THEME_COLOR"])
    args = parser.parse_args()

    brand_vars = {
        "BRAND_NAME": args.brand_name,
        "BRAND_AP_PREFIX": args.brand_ap_prefix,
        "BRAND_AP_PASSWORD": args.brand_ap_password,
        "BRAND_MODEL": args.brand_model,
        "BRAND_THEME_COLOR": args.brand_theme_color,
    }

    embed(args.input, args.output, brand_vars)


if __name__ == "__main__":
    main()
