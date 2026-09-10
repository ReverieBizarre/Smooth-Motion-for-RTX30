#!/usr/bin/env python3
"""extract_dlssg_pack.py - stage 0 of the hybrid plan.

Lifts every artefact a DLSSG host needs out of a backport host PE (or any
binary that carries the 310.x pack) and writes them to a directory, grouped by
GPU architecture, with the kernel entry name as the filename:

    <out>/plan.json                 the 140-op dispatch plan
    <out>/manifest.json             weight manifest (key/offset/bytes/sha256)
    <out>/weights.bin               the weight blob
    <out>/cubins/sm_86/<entry>.cubin
    <out>/ptx/sm_86/<entry>.ptx
    <out>/inventory.json            what was found, with sizes and hashes

The point of stage 0 is to prove the pack is liftable and to answer, up front,
whether the target architecture is present.  Whether the model can then be
*driven* is a separate question: the pack describes geometry, not semantics, so
the host still has to resolve the symbolic dimensions and bind the NGX inputs.

NOTE: the extracted weights and cubins are NVIDIA's.  Keep them local; do not
redistribute.

usage: extract_dlssg_pack.py <pe_file> <out_dir>
"""
import hashlib
import json
import os
import re
import struct
import sys

CONTAINER_MAGIC = b'\x50\xed\x55\xba'          # 0xBA55ED50
SM_KNOWN = (70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 110, 120, 121)


def walk_resources(f):
    pe = struct.unpack_from('<I', f, 0x3c)[0]
    nsec = struct.unpack_from('<H', f, pe + 6)[0]
    optsz = struct.unpack_from('<H', f, pe + 20)[0]
    secs = []
    base = pe + 24 + optsz
    for i in range(nsec):
        o = base + 40 * i
        vs, va, rs, ro = struct.unpack_from('<IIII', f, o + 8)
        secs.append((va, vs, ro, rs))

    def r2o(rva):
        for va, vs, ro, rs in secs:
            if va <= rva < va + max(vs, rs):
                return ro + (rva - va)
        return None

    dd = pe + 24 + 112
    ro = r2o(struct.unpack_from('<I', f, dd + 16)[0])
    if ro is None:
        return {}

    def entries(off):
        nn, ni = struct.unpack_from('<HH', f, off + 12)
        return [struct.unpack_from('<II', f, off + 16 + 8 * i) for i in range(nn + ni)]

    out = {}
    for typ, offv in entries(ro):
        if typ != 10:
            continue
        for rid, offv2 in entries(ro + (offv & 0x7fffffff)):
            langs = entries(ro + (offv2 & 0x7fffffff))
            if not langs:
                continue
            drva, dsz = struct.unpack_from('<II', f, ro + (langs[0][1] & 0x7fffffff))
            o = r2o(drva)
            if o is not None:
                out[rid] = (o, dsz)
    return out


def elf_arch(buf, off):
    if buf[off:off + 4] != b'\x7fELF' or buf[off + 4] != 2:
        return None, []
    flags = struct.unpack_from('<I', buf, off + 0x30)[0]
    sm = 0
    for shift in (8, 16, 0, 24):
        v = (flags >> shift) & 0xff
        if v in SM_KNOWN:
            sm = v
            break
    try:
        shoff = struct.unpack_from('<Q', buf, off + 0x28)[0]
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', buf, off + 0x3a)
        st = off + shoff + shentsize * shstrndx
        soff = struct.unpack_from('<Q', buf, st + 0x18)[0]
        ssz = struct.unpack_from('<Q', buf, st + 0x20)[0]
        names = [s.decode('latin1') for s in buf[off + soff:off + soff + ssz].split(b'\x00')]
        return sm, [n[6:] for n in names if n.startswith('.text.')]
    except Exception:
        return sm, []


def elf_size(buf, off):
    """total byte length of the ELF image starting at off (sections + shdr table)"""
    shoff = struct.unpack_from('<Q', buf, off + 0x28)[0]
    shentsize, shnum = struct.unpack_from('<HH', buf, off + 0x3a)
    end = shoff + shentsize * shnum
    for i in range(shnum):
        sh = off + shoff + shentsize * i
        typ = struct.unpack_from('<I', buf, sh + 4)[0]
        if typ == 8:                              # SHT_NOBITS
            continue
        so = struct.unpack_from('<Q', buf, sh + 0x18)[0]
        ss = struct.unpack_from('<Q', buf, sh + 0x20)[0]
        end = max(end, so + ss)
    return end


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, outdir = sys.argv[1], sys.argv[2]
    f = open(src, 'rb').read()
    print("source: %s (%d bytes)" % (src, len(f)))
    res = walk_resources(f)
    print("RCDATA resources: %d" % len(res))
    if not res:
        return 1

    os.makedirs(outdir, exist_ok=True)
    inv = {"source": os.path.basename(src), "source_bytes": len(f),
           "resources": len(res), "artifacts": []}

    def sha(b):
        return hashlib.sha256(b).hexdigest()

    # ---- single-purpose resources: plan / manifest / weights ----
    for rid, (o, sz) in sorted(res.items()):
        blob = f[o:o + sz]
        if blob[:1] != b'{':
            continue
        try:
            j = json.loads(blob.decode('utf-8'))
        except Exception:
            continue
        if 'phases' in j:
            p = os.path.join(outdir, 'plan.json')
            open(p, 'wb').write(blob)
            inv['artifacts'].append({"id": rid, "kind": "dispatch_plan",
                                     "path": "plan.json", "bytes": sz, "sha256": sha(blob)})
            print("  plan.json      resource %-6s %9d B  %s" % (rid, sz, sha(blob)[:16]))
        elif 'weights' in j:
            p = os.path.join(outdir, 'manifest.json')
            open(p, 'wb').write(blob)
            inv['artifacts'].append({"id": rid, "kind": "weight_manifest",
                                     "path": "manifest.json", "bytes": sz, "sha256": sha(blob)})
            print("  manifest.json  resource %-6s %9d B  %s" % (rid, sz, sha(blob)[:16]))

    # the weight blob is the largest non-JSON, non-ELF, high-entropy blob
    biggest = None
    for rid, (o, sz) in sorted(res.items()):
        blob = f[o:o + sz]
        if blob[:1] == b'{' or blob[:4] == b'\x7fELF' or blob[:4] == CONTAINER_MAGIC:
            continue
        if biggest is None or sz > biggest[1]:
            biggest = (rid, sz, o)
    if biggest:
        rid, sz, o = biggest
        blob = f[o:o + sz]
        open(os.path.join(outdir, 'weights.bin'), 'wb').write(blob)
        inv['artifacts'].append({"id": rid, "kind": "weights", "path": "weights.bin",
                                 "bytes": sz, "sha256": sha(blob)})
        print("  weights.bin    resource %-6s %9d B  %s" % (rid, sz, sha(blob)[:16]))

    # ---- cubins and PTX, named by kernel entry, grouped by arch ----
    counts = {}
    for rid, (o, sz) in sorted(res.items()):
        blob = f[o:o + sz]

        if blob[:4] == b'\x7fELF':
            sm, kernels = elf_arch(blob, 0)
            n = elf_size(blob, 0)
            for k in (kernels or ["unnamed_%d" % rid]):
                d = os.path.join(outdir, 'cubins', 'sm_%d' % sm)
                os.makedirs(d, exist_ok=True)
                open(os.path.join(d, k + '.cubin'), 'wb').write(blob[:n])
            counts.setdefault('cubins', {}).setdefault(sm, 0)
            counts['cubins'][sm] += 1

        elif blob[:4] == CONTAINER_MAGIC:
            # container: 16-byte header then an ELF or PTX payload
            body = blob[16:]
            if body[:4] == b'\x7fELF':
                sm, kernels = elf_arch(body, 0)
                n = elf_size(body, 0)
                for k in (kernels or ["unnamed_%d" % rid]):
                    d = os.path.join(outdir, 'cubins', 'sm_%d' % sm)
                    os.makedirs(d, exist_ok=True)
                    open(os.path.join(d, k + '.cubin'), 'wb').write(body[:n])
                counts.setdefault('packed_cubins', {}).setdefault(sm, 0)
                counts['packed_cubins'][sm] += 1
            else:
                m = re.search(rb'\.target\s+(sm_\d+)', body)
                tgt = m.group(1).decode() if m else 'unknown'
                m2 = re.search(rb'\.entry\s+(\w+)', body)
                nm = m2.group(1).decode() if m2 else 'ptx_%d' % rid
                d = os.path.join(outdir, 'ptx', tgt)
                os.makedirs(d, exist_ok=True)
                open(os.path.join(d, nm + '.ptx'), 'wb').write(body)
                counts.setdefault('ptx', {}).setdefault(tgt, 0)
                counts['ptx'][tgt] += 1

    print("\n--- extracted ---")
    for kind, per in sorted(counts.items()):
        for arch, n in sorted(per.items(), key=lambda kv: str(kv[0])):
            print("  %-14s %-8s x%d" % (kind, arch, n))

    inv['counts'] = {k: {str(a): n for a, n in v.items()} for k, v in counts.items()}
    open(os.path.join(outdir, 'inventory.json'), 'w').write(json.dumps(inv, indent=2))
    print("\nwrote %s" % outdir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
