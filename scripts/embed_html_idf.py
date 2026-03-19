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
import re
import sys


def _minify_css(css: str) -> str:
    """Remove comments, collapse whitespace, strip unnecessary chars."""
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.DOTALL)
    css = re.sub(r"\s+", " ", css)
    css = re.sub(r"\s*([{}:;,>~+])\s*", r"\1", css)
    css = re.sub(r";\s*}", "}", css)
    return css.strip()


def _minify_js(js: str) -> str:
    """Strip leading whitespace, blank lines, and line comments."""
    out = []
    for line in js.split("\n"):
        s = line.strip()
        if not s:
            continue
        # Remove full-line // comments (but not URLs like http://)
        if re.match(r"^//(?!/)", s):
            continue
        out.append(s)
    return "\n".join(out)


def _minify_html(html: str) -> str:
    """Minify inline CSS/JS and collapse inter-tag whitespace."""
    html = re.sub(
        r"<style>(.*?)</style>",
        lambda m: "<style>" + _minify_css(m.group(1)) + "</style>",
        html,
        flags=re.DOTALL,
    )
    html = re.sub(
        r"<script>(.*?)</script>",
        lambda m: "<script>" + _minify_js(m.group(1)) + "</script>",
        html,
        flags=re.DOTALL,
    )
    html = re.sub(r">\s+<", "><", html)
    return html


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

    # Minify inline CSS, JS, and inter-tag whitespace
    pre_min = len(text)
    text = _minify_html(text)
    raw = text.encode("utf-8")

    compressed = gzip.compress(raw, compresslevel=9)
    raw_size = len(raw)
    gz_size = len(compressed)

    os.makedirs(os.path.dirname(os.path.abspath(output_file)), exist_ok=True)
    with open(output_file, "wb") as f:
        f.write(compressed)

    print(f"Embedded {input_file}")
    print(f"  Original:    {pre_min:,} bytes")
    print(f"  Minified:    {raw_size:,} bytes ({pre_min - raw_size:,} saved)")
    print(f"  Gzipped:     {gz_size:,} bytes")
    print(f"  Ratio:       {gz_size / pre_min * 100:.1f}%")
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
