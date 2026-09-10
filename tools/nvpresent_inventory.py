# ---------------------------------------------------------------------------
#  Authoritative cubin inventory + e_flags decoding.
#
#  Two analyses of NvPresent64.dll disagree about what the 74 embedded ELF
#  blobs are.  This settles it by listing every blob with its e_flags and its
#  kernel entry name, and by calibrating the e_flags field layout against
#  cubins whose architecture is known independently (the DLSSG 310.1 pack
#  contains sm_86 and sm_75 subdirectories that were named from a different
#  code path).
# ---------------------------------------------------------------------------
import os
import re
import struct
import sys

NVPRESENT = (r'C:\Windows\System32\DriverStore\FileRepository'
             r'\nv_dispi.inf_amd64_a3944b54ff18b284\NvPresent64.dll')
KNOWN = (r'C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\dlssg_pack\cubins')

KNOWN_SM = {75, 80, 86, 87, 89, 90, 100, 101, 120, 121}


def elf_at(buf, o):
    """Return (e_flags, e_machine, [kernel entry names], total_size) for the
    ELF image starting at buf[o]."""
    e_machine = struct.unpack_from('<H', buf, o + 0x12)[0]
    e_flags = struct.unpack_from('<I', buf, o + 0x30)[0]
    shoff = struct.unpack_from('<Q', buf, o + 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', buf, o + 0x3a)
    phoff = struct.unpack_from('<Q', buf, o + 0x20)[0]
    phentsize, phnum = struct.unpack_from('<HH', buf, o + 0x36)

    end = max(shoff + shentsize * shnum, phoff + phentsize * phnum)
    st = o + shoff + shentsize * shstrndx
    soff = struct.unpack_from('<Q', buf, st + 0x18)[0]
    ssz = struct.unpack_from('<Q', buf, st + 0x20)[0]
    names = [s.decode('latin1') for s in buf[o + soff:o + soff + ssz].split(b'\x00')]

    kernels = [n[6:] for n in names if n.startswith('.text.')]
    # also pick up .nv.info.<kernel> names, which exist even for kernels whose
    # .text section got merged
    info = [n[9:] for n in names if n.startswith('.nv.info.') and n != '.nv.info.']

    for i in range(shnum):
        sh = o + shoff + shentsize * i
        so = struct.unpack_from('<Q', buf, sh + 0x18)[0]
        sz = struct.unpack_from('<Q', buf, sh + 0x20)[0]
        end = max(end, so + sz)

    return e_flags, e_machine, sorted(set(kernels)), sorted(set(info)), end - o


def decode_sm(flags):
    """Which byte of e_flags holds the SM.  Calibrated empirically below."""
    for shift in (16, 0, 8, 24):
        v = (flags >> shift) & 0xff
        if v in KNOWN_SM:
            return v, shift
    return None, None


print('=' * 78)
print('STEP 1  calibrate e_flags against cubins with independently known arch')
print('=' * 78)
calib = {}
for arch in ('sm_75', 'sm_86'):
    d = os.path.join(KNOWN, arch)
    if not os.path.isdir(d):
        continue
    for fn in sorted(os.listdir(d))[:2]:
        b = open(os.path.join(d, fn), 'rb').read()
        fl, mach, k, inf, size = elf_at(b, 0)
        sm, sh = decode_sm(fl)
        calib[arch] = (fl, sm, sh)
        print('  %-6s %-34s e_flags=%#010x -> decoded sm_%s (byte %d)  e_machine=%#x'
              % (arch, fn, fl, sm, sh, mach))
print('  so the SM lives in byte %s of e_flags for these builds'
      % {v[2] for v in calib.values()})

print()
print('=' * 78)
print('STEP 2  every embedded image in NvPresent64.dll')
print('=' * 78)
buf = open(NVPRESENT, 'rb').read()
print('file size: %d bytes' % len(buf))

offs = [m.start() for m in re.finditer(rb'\x7fELF', buf)]
rows = []
for o in offs:
    try:
        fl, mach, k, inf, size = elf_at(buf, o)
    except Exception as e:
        print('  blob at %#x: parse failed (%s)' % (o, e))
        continue
    sm, sh = decode_sm(fl)
    name = (k or inf or ['?'])[0]
    rows.append((o, fl, sm, mach, size, name, k, inf))

print('total ELF magic hits: %d' % len(offs))
print('parsed              : %d' % len(rows))
print()

from collections import Counter, defaultdict
by_arch = defaultdict(list)
for o, fl, sm, mach, size, name, k, inf in rows:
    by_arch[sm].append(name)

print('by decoded architecture:')
for sm in sorted(by_arch, key=lambda x: (x is None, x)):
    names = by_arch[sm]
    print('  sm_%-5s  n=%3d' % (sm, len(names)))

print()
print('kernel-name split (looking for _fp8 twins within one architecture):')
for sm in sorted(by_arch, key=lambda x: (x is None, x)):
    names = sorted(set(by_arch[sm]))
    fp8 = [n for n in names if 'fp8' in n.lower()]
    plain = [n for n in names if 'fp8' not in n.lower()]
    print('  --- sm_%s: %d distinct names, %d containing "fp8", %d not'
          % (sm, len(names), len(fp8), len(plain)))
    print('      fp8   : %s' % (', '.join(fp8) if fp8 else '(none)'))
    print('      plain : %s' % (', '.join(plain) if plain else '(none)'))

print()
print('all distinct kernel entry names, with the architectures carrying them:')
alln = defaultdict(set)
for o, fl, sm, mach, size, name, k, inf in rows:
    for n in set(k) | set(inf):
        alln[n].add(sm)
for n in sorted(alln):
    print('  %-34s %s' % (n, sorted(alln[n], key=lambda x: (x is None, x))))
