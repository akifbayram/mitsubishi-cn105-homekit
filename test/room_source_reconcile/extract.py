#!/usr/bin/env python3
"""Lift the real room-source functions out of espnow_link.cpp for a host build."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()

def function(name):
    match = re.search(r'^static [^\n;{]*\b' + re.escape(name) + r'\(', source, re.M)
    if not match:
        sys.exit(f'{name}() not found in {sys.argv[1]}')
    brace = source.index('{', match.start())
    level = 1
    end = brace + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

Path(sys.argv[2]).write_text('\n\n'.join(function(name) for name in sys.argv[3:]) + '\n')
