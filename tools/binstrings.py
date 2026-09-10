#!/usr/bin/env python3
"""binstrings.py - extract ASCII + UTF-16LE strings from a binary and filter.

usage: binstrings.py <file> [regex] [--min N] [--limit N] [--ctx N]
"""
import re
import sys
import os

def strings(buf, minlen, wide):
    pat = rb'(?:[\x20-\x7e]\x00){%d,}' % minlen if wide else rb'[\x20-\x7e]{%d,}' % minlen
    out = []
    for m in re.finditer(pat, buf):
        s = m.group()
        out.append(s.decode('utf-16le' if wide else 'latin1'))
    return out

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    rx = None
    minlen = 6
    limit = 80
    i = 1
    while i < len(args):
        a = args[i]
        if a == '--min':
            i += 1; minlen = int(args[i])
        elif a == '--limit':
            i += 1; limit = int(args[i])
        elif rx is None:
            rx = re.compile(a, re.I)
        i += 1

    buf = open(path, 'rb').read()
    print(f"file   : {path}  ({len(buf):,} bytes)")
    seen = {}
    for wide in (False, True):
        for s in strings(buf, minlen, wide):
            try:
                s.encode('ascii')
            except UnicodeEncodeError:
                continue
            if rx and not rx.search(s):
                continue
            key = s
            if key not in seen:
                seen[key] = 'w' if wide else 'a'
    print(f"hits   : {len(seen)}")
    for s, w in sorted(seen.items())[:limit]:
        print(f"  [{w}] {s}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
