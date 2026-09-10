#!/usr/bin/env python3
"""scan_blobs.py - find embedded CUDA cubins (ELF) and PTX inside a PE/DLL and
report their SM targets plus kernel entry names.

usage: scan_blobs.py <file> [--kernels] [--max N]
"""
import re
import struct
import sys
from collections import Counter, defaultdict


def elf_info(buf, off):
    """return (e_machine, sm, flags, section_names) or None"""
    if buf[off:off + 4] != b'\x7fELF':
        return None
    cls = buf[off + 4]
    if cls != 2:                     # want ELF64
        return None
    machine = struct.unpack_from('<H', buf, off + 2)[0]
    flags = struct.unpack_from('<I', buf, off + 0x30)[0]
    try:
        shoff = struct.unpack_from('<Q', buf, off + 0x28)[0]
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', buf, off + 0x3a)
    except struct.error:
        return machine, 0, flags, []
    sm = 0
    for shift in (8, 16, 0, 24):     # e_flags SM field has drifted across ptxas versions
        v = (flags >> shift) & 0xff
        if v in (70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 110, 120, 121):
            sm = v
            break
    names = []
    if shoff and shnum and shstrndx < shnum:
        st = off + shoff + shentsize * shstrndx
        soff = struct.unpack_from('<Q', buf, st + 0x18)[0]
        ssz = struct.unpack_from('<Q', buf, st + 0x20)[0]
        raw = buf[off + soff: off + soff + ssz]
        names = [s.decode('latin1') for s in raw.split(b'\x00') if s]
    return machine, sm, flags, names


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    show_kernels = '--kernels' in args
    maxn = 40
    if '--max' in args:
        maxn = int(args[args.index('--max') + 1])

    buf = open(path, 'rb').read()
    print(f"file : {path}  ({len(buf):,} bytes)")

    # ---- embedded cubins ----
    offs = [m.start() for m in re.finditer(rb'\x7fELF', buf)]
    infos = []
    for o in offs:
        i = elf_info(buf, o)
        if i:
            infos.append((o, i))
    print(f"ELF blobs found : {len(infos)}")
    arch = Counter()
    kernels = defaultdict(int)
    for o, (machine, sm, flags, names) in infos:
        arch[sm] += 1
        for n in names:
            if n.startswith('.text.'):
                kernels[n[6:]] += 1
    for sm, n in sorted(arch.items()):
        print(f"   sm_{sm:<4} x{n}")
    if show_kernels and kernels:
        print(f"\nkernel entries ({len(kernels)}):")
        for k in sorted(kernels):
            print(f"   {k}")

    # ---- PTX ----
    ptx = set(re.findall(rb'\.target\s+(sm_\d+|compute_\d+)', buf))
    print(f"\nPTX .target values : {sorted(x.decode() for x in ptx) or 'none'}")
    ver = set(re.findall(rb'\.version\s+(\d+\.\d+)', buf))
    print(f"PTX .version       : {sorted(x.decode() for x in ver) or 'none'}")

    # ---- fatbin / nvrtc ----
    for magic, label in ((b'\x50\xed\x55\xba', 'fatbin/container (0xBA55ED50)'),
                         (b'\xb1CbF', 'fatbin  (0x466243b1)')):
        print(f"{label:<34}: {buf.count(magic)}")
    for s in (b'nvrtc', b'cuModuleLoadData', b'CUDA C++'):
        print(f"{s.decode():<34}: {buf.count(s)}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
