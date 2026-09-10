# ---------------------------------------------------------------------------
#  SASS opcode vocabulary comparison.
#
#  A successful cuModuleLoadData proves the loader accepted the image; it does
#  not prove the SASS contains nothing Ampere lacks.  This answers the static
#  half directly: build the opcode vocabulary of cubins whose architecture is
#  independently known (the DLSSG 310.1 pack ships sm_75 and sm_86 builds), then
#  ask which opcodes NvPresent64's sm_89 kernels use that never appear in any
#  genuine sm_86 build.
# ---------------------------------------------------------------------------
import os
import re
import subprocess
import sys
from collections import defaultdict

PY = sys.executable
NVDISASM = r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvdisasm.exe'
NVPRESENT = (r'C:\Windows\System32\DriverStore\FileRepository'
             r'\nv_dispi.inf_amd64_a3944b54ff18b284\NvPresent64.dll')
KNOWN = r'C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\dlssg_pack\cubins'
OUT = r'C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\build\sass_scratch'

OPLINE = re.compile(r'/\*[0-9a-f]{4,}\*/\s+(@!?U?P\d+\s+)?([A-Z][A-Z0-9._]*)')


def ops_of(path):
    """Opcode multiset of a cubin, via nvdisasm."""
    try:
        r = subprocess.run([NVDISASM, '-c', path], capture_output=True,
                           text=True, timeout=120, errors='replace')
    except Exception:
        return None
    if r.returncode != 0:
        return None
    ops = []
    for line in r.stdout.splitlines():
        m = OPLINE.search(line)
        if m:
            op = m.group(2).split('.')[0]
            ops.append(op)
    return ops


def vocab(paths):
    v = set()
    per = {}
    for p in paths:
        o = ops_of(p)
        if o is None:
            continue
        per[os.path.basename(p)] = o
        v |= set(o)
    return v, per


print('=' * 78)
print('building vocabulary from cubins with independently known architecture')
print('=' * 78)
known_paths = {}
for arch in ('sm_75', 'sm_86'):
    d = os.path.join(KNOWN, arch)
    known_paths[arch] = [os.path.join(d, f) for f in sorted(os.listdir(d))
                         if f.endswith('.cubin')]
    print('  %s : %d cubins' % (arch, len(known_paths[arch])))

os.makedirs(OUT, exist_ok=True)
voc = {}
for arch, paths in known_paths.items():
    v, per = vocab(paths)
    voc[arch] = v
    print('  %s : %d distinct opcodes' % (arch, len(v)))

sm86_vocab = voc['sm_86']

print()
print('=' * 78)
print('extracting every embedded kernel from NvPresent64.dll')
print('=' * 78)
buf = open(NVPRESENT, 'rb').read()
offs = [m.start() for m in re.finditer(b'\x7fELF', buf)]
print('  %d ELF blobs' % len(offs))

import struct


def extract(o, tag):
    # NOTE: e_shoff / e_phoff / section offsets are relative to the start of the
    # embedded image, so every one of them needs `o` added to become a file
    # offset in the DLL.  Forgetting that writes zero-length files.
    shoff = struct.unpack_from('<Q', buf, o + 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', buf, o + 0x3a)
    phoff = struct.unpack_from('<Q', buf, o + 0x20)[0]
    phentsize, phnum = struct.unpack_from('<HH', buf, o + 0x36)
    end = max(shoff + shentsize * shnum, phoff + phentsize * phnum)
    st = shoff + shentsize * shstrndx
    soff = struct.unpack_from('<Q', buf, o + st + 0x18)[0]
    ssz = struct.unpack_from('<Q', buf, o + st + 0x20)[0]
    names = [s.decode('latin1') for s in
             buf[o + soff:o + soff + ssz].split(b'\x00')]
    ks = [n[6:] for n in names if n.startswith('.text.')]
    for i in range(shnum):
        sh = shoff + shentsize * i
        so = struct.unpack_from('<Q', buf, o + sh + 0x18)[0]
        sz = struct.unpack_from('<Q', buf, o + sh + 0x20)[0]
        end = max(end, so + sz)
    fn = os.path.join(OUT, '%s_%s.cubin' % (tag, ks[0] if ks else 'unknown'))
    with open(fn, 'wb') as f:
        f.write(buf[o:o + end])
    return fn, (ks[0] if ks else 'unknown')


rows = []
for o in offs:
    fl = struct.unpack_from('<I', buf, o + 0x30)[0]
    arch = {0x590559: 'sm_89', 0x6007802: 'sm_120'}.get(fl, 'arch_%x' % fl)
    fn, kname = extract(o, '%s' % arch)
    rows.append((arch, kname, fn))

print('  extracted %d' % len(rows))

print()
print('=' * 78)
print('which sm_89 opcodes never appear in any genuine sm_86 build?')
print('=' * 78)

alien_by_kernel = defaultdict(set)
for arch, kname, fn in rows:
    o = ops_of(fn)
    if o is None:
        print('  nvdisasm failed on %s' % kname)
        continue
    alien = set(o) - sm86_vocab
    if alien:
        alien_by_kernel[kname] = alien

if not alien_by_kernel:
    print('  NONE.  Every opcode used by every embedded kernel also occurs in at')
    print('  least one genuine sm_86 cubin on this machine.')
else:
    for k in sorted(alien_by_kernel):
        print('  %-26s %s' % (k, sorted(alien_by_kernel[k])))

print()
print('=' * 78)
print('tensor / FP16 / FP8 instruction census')
print('=' * 78)
interesting = ('HMMA', 'QMMA', 'OMMA', 'IMMA', 'HFMA2', 'HADD2', 'HMUL2',
               'F2FP', 'F2F', 'LDGSTS', 'LDSM')
hdr = '  %-26s %-7s %s' % ('kernel', 'arch', ' '.join('%-6s' % i for i in interesting))
print(hdr)
print('  ' + '-' * (len(hdr) - 2))

for arch, kname, fn in sorted(rows, key=lambda r: (r[0], r[1])):
    o = ops_of(fn)
    if o is None:
        continue
    from collections import Counter
    c = Counter(o)
    # FP8 shows up as typed operand suffixes rather than an opcode, so also
    # look at the raw text for E4M3/E5M2 forms
    r = subprocess.run([NVDISASM, '-c', fn], capture_output=True, text=True,
                       timeout=120, errors='replace')
    fp8 = ('E4M3' in r.stdout) or ('E5M2' in r.stdout) or ('FP8' in r.stdout)
    cells = ' '.join('%-6d' % c.get(i, 0) for i in interesting)
    print('  %-26s %-7s %s%s' % (kname, arch, cells, '   <-- FP8 operand' if fp8 else ''))

print()
print('  note: "HMMA" here is the base mnemonic after stripping the operand')
print('  suffixes, so an FP8 MMA and an FP16 MMA both count as HMMA.  The FP8')
print('  column above is the raw-text check for E4M3/E5M2 typed operands.')
