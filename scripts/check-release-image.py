#!/usr/bin/env python3
"""Reject release artifacts whose embedded ESP-IDF version differs from the tag."""
import argparse
from pathlib import Path
import struct
import sys


def check_version(path, expected):
    # ESP-IDF puts esp_app_desc_t at the start of the first image segment:
    # 24-byte image header + 8-byte segment header + 16-byte descriptor prefix.
    data = Path(path).read_bytes()[:288]
    if len(data) != 288 or data[0] != 0xE9 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("missing ESP-IDF application descriptor")
    raw, terminator, _ = data[48:80].partition(b"\0")
    if not raw or not terminator:
        raise ValueError("missing or unterminated embedded version")
    actual = raw.decode("ascii")
    if actual != expected:
        raise ValueError(f"embedded version {actual!r} differs from release tag {expected!r}")
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("tag")
    args = parser.parse_args()
    try:
        version = check_version(args.image, args.tag)
    except (OSError, ValueError) as error:
        print(f"::error::{args.image}: {error}", file=sys.stderr)
        return 1
    print(f"{args.image}: embedded release version {version} verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
