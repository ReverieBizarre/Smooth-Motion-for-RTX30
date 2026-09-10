#!/usr/bin/env python3
"""scan_kernels_by_arch.py - for a package with multi-arch cubins, list which
kernel entry points exist per GPU architecture.

usage: scan_kernels_by_arch.py <file>
"""
import re
import struct
import sys
from collections import defaultdict

SM_KNOWN = (70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 110, 120, 121)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    buf = open(path, 'rb').read()
    print("file: %s (%d bytes)" % (path, len(buf)))

    per_arch = defaultdict(set)
    arch_count = defaultdict(int)

    for m in re.finditer(rb'\x7fELF', buf):
        off = m.start()
        if buf[off + 4] != 2:
            continue
        flags = struct.unpack_from('<I', buf, off + 0x30)[0]
        sm = 0
        for shift in (8, 16, 0, 24):
            v = (flags >> shift) & 0xff
            if v in SM_KNOWN:
                sm = v
                break
        arch_count[sm] += 1
        try:
            shoff = struct.unpack_from('<Q', buf, off + 0x28)[0]
            shentsize, shnum, shstrndx = struct.unpack_from('<HHH', buf, off + 0x3a)
        except struct.error:
            continue
        if not shoff or shstrndx >= shnum:
            continue
        st = off + shoff + shentsize * shstrndx
        soff = struct.unpack_from('<Q', buf, st + 0x18)[0]
        ssz = struct.unpack_from('<Q', buf, st + 0x20)[0]
        raw = buf[off + soff: off + soff + ssz]
        for s in raw.split(b'\x00'):
            s = s.decode('latin1')
            if s.startswith('.text.'):
                per_arch[sm].add(s[6:])

    print("\ncubin count per arch: " +
          "  ".join("sm_%d=%d" % (k, v) for k, v in sorted(arch_count.items())))

    allk = set()
    for v in per_arch.values():
        allk |= v

    print("\nkernel entries (%d unique):" % len(allk))
    arches = sorted(per_arch)
    hdr = "  %-46s " % "kernel"
    print(hdr + " ".join("sm_%d" % a for a in arches))
    print("  " + "-" * (47 + 6 * len(arches)))
    for k in sorted(allk):
        row = "  %-46s " % (k[:46])
        print(row + " ".join("  y  " if k in per_arch[a] else "  .  " for a in arches))

    # what is unique to each arch
    print("\narch-exclusive kernels:")
    for a in arches:
        others = set()
        for b in arches:
            if b != a:
                others |= per_arch[b]
        only = sorted(per_arch[a] - others)
        print("  sm_%d: %d exclusive%s" % (a, len(only), (": " + ", ".join(only[:14])) if only else ""))
    return 0


if __name__ == '__main__':
    sys.exit(main())
