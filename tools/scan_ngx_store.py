#!/usr/bin/env python3
"""scan_ngx_store.py - walk the NVIDIA NGX model store and report, for every
DLSS-family package, which GPU architectures it actually carries kernels for.

The store lives at C:\\ProgramData\\NVIDIA\\NGX\\models and holds the model
packages that the NVIDIA App pulls down per feature:

    <feature>/versions/<ver>/files/<type><hash>.bin     model package
    sl_<feature>_0/versions/<ver>/files/<type><hash>.dll   runtime DLL

Version numbers for the dlssg family decode as  major<<16 | minor<<8 | patch,
so 20316416 -> 310.1.0.
"""
import os
import re
import struct
import sys
from collections import Counter, defaultdict

ROOT = r"C:\ProgramData\NVIDIA\NGX\models"
SM_KNOWN = (70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 110, 120, 121)


def decode_ver(n):
    try:
        n = int(n)
    except ValueError:
        return str(n)
    return "%d.%d.%d" % (n >> 16, (n >> 8) & 0xff, n & 0xff)


def elf_arch(buf, off):
    if buf[off:off + 4] != b'\x7fELF' or buf[off + 4] != 2:
        return None
    flags = struct.unpack_from('<I', buf, off + 0x30)[0]
    for shift in (8, 16, 0, 24):
        v = (flags >> shift) & 0xff
        if v in SM_KNOWN:
            return v
    return 0


def analyse(path):
    buf = open(path, 'rb').read()
    arch = Counter()
    for m in re.finditer(rb'\x7fELF', buf):
        a = elf_arch(buf, m.start())
        if a is not None:
            arch[a] += 1
    ptx = sorted(set(x.decode() for x in re.findall(rb'\.target\s+(sm_\d+|compute_\d+)', buf)))
    return len(buf), arch, ptx


def main():
    rows = []
    for feat in sorted(os.listdir(ROOT)):
        vdir = os.path.join(ROOT, feat, 'versions')
        if not os.path.isdir(vdir):
            continue
        for ver in sorted(os.listdir(vdir), key=lambda s: int(s) if s.isdigit() else 0):
            fdir = os.path.join(vdir, ver, 'files')
            if not os.path.isdir(fdir):
                continue
            for root, _dirs, files in os.walk(fdir):
                for f in files:
                    if not f.lower().endswith(('.bin', '.dll')):
                        continue
                    p = os.path.join(root, f)
                    try:
                        size, arch, ptx = analyse(p)
                    except Exception as e:
                        print("  !! %s: %s" % (p, e))
                        continue
                    if arch or ptx:
                        rows.append((feat, ver, f, size, arch, ptx))

    if not rows:
        print("no packages with embedded kernels found")
        return 0

    print("%-22s %-10s %-24s %10s  %s" %
          ("feature", "ver", "file", "size", "architectures"))
    print("-" * 108)
    for feat, ver, f, size, arch, ptx in rows:
        a = " ".join("sm_%d x%d" % (k, v) for k, v in sorted(arch.items())) or "-"
        print("%-22s %-10s %-24s %10d  %s" % (feat, ver, f, size, a))
        if ptx:
            print("%-22s %-10s %-24s %10s  PTX: %s" % ("", "", "", "", ", ".join(ptx)))

    # summary per feature: which SM builds exist at all
    print("\n=== per-feature architecture coverage ===")
    cov = defaultdict(Counter)
    for feat, ver, f, size, arch, ptx in rows:
        cov[feat].update(arch)
    for feat in sorted(cov):
        total = sum(cov[feat].values())
        has86 = cov[feat].get(86, 0)
        print("  %-22s total cubins %-5d  sm_86 %-5d %s" %
              (feat, total, has86, "AMPERA-CAPABLE" if has86 else "no Ampere build"))
    return 0


if __name__ == '__main__':
    sys.exit(main())
