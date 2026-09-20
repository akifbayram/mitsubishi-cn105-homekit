#!/usr/bin/env python3
"""Compile the real WebUI broadcast boundary with a deliberately wedged socket."""
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
def method(name):
    start = source.index('void WebUI::' + name + '(')
    brace = source.index('{', start)
    level = 1
    end = brace + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
Path(sys.argv[2]).write_text(method('sendWsText') + '\n' + method('broadcastWs') + '\n')
