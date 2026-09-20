#!/usr/bin/env python3
"""Compile the real WebUI broadcast boundary with a deliberately wedged socket."""
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
def method(name):
    starts = [source.find(prefix + ' WebUI::' + name + '(') for prefix in ('void', 'bool')]
    start = min(position for position in starts if position >= 0)
    brace = source.index('{', start)
    level = 1
    end = brace + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
Path(sys.argv[2]).write_text(method('sendWsText') + '\n' + method('broadcastWs') + '\n')

if len(sys.argv) > 3:
    Path(sys.argv[3]).write_text(method('loop') + '\n')
