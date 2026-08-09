#!/usr/bin/env bash
# Every {{PLACEHOLDER}} in web/*.html must be substituted by
# scripts/embed_html_idf.py. The script is a plain str.replace loop with no
# validation, so an unknown or misspelled key is silently left in the output
# and ships literally to the browser.
set -euo pipefail
cd "$(dirname "$0")/../.."

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
for f in web/*.html; do
    out="$tmp/$(basename "$f").gz"
    python3 scripts/embed_html_idf.py --input "$f" --output "$out" >/dev/null
    gzip -dc "$out" > "$tmp/decoded"
    leftover=$(grep -o '{{[A-Z_]*}}' "$tmp/decoded" | sort -u || true)
    if [ -n "$leftover" ]; then
        echo "FAIL: $f ships unsubstituted placeholders:"
        echo "$leftover" | sed 's/^/    /'
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "PASS: web/*.html placeholders all substituted"
fi
exit "$fail"
