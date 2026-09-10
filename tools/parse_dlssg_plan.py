#!/usr/bin/env python3
"""parse_dlssg_plan.py - extract and describe the dispatch plan / weight manifest
embedded in a DLSSG backport host (or any PE that carries the 310.x plan JSON).

Reports, in order:
  * resource inventory
  * plan schema + geometry ABI
  * phase / op-kind histogram
  * the distinct kernel entries
  * the buffer table
  * every string in the plan that names an external input, which is the thing
    that decides whether the model can be driven from a back buffer alone.

usage: parse_dlssg_plan.py <pe_file> [--buffers] [--ops]
"""
import json
import re
import struct
import sys
from collections import Counter


def walk_resources(f):
    pe = struct.unpack_from('<I', f, 0x3c)[0]
    nsec = struct.unpack_from('<H', f, pe + 6)[0]
    optsz = struct.unpack_from('<H', f, pe + 20)[0]

    secs = []
    base = pe + 24 + optsz
    for i in range(nsec):
        o = base + 40 * i
        name = f[o:o + 8].rstrip(b'\0').decode('latin1')
        vs, va, rs, ro = struct.unpack_from('<IIII', f, o + 8)
        secs.append((name, va, vs, ro, rs))

    def r2o(rva):
        for _n, va, vs, ro, rs in secs:
            if va <= rva < va + max(vs, rs):
                return ro + (rva - va)
        return None

    dd = pe + 24 + 112
    rrva = struct.unpack_from('<I', f, dd + 16)[0]
    ro = r2o(rrva)
    if ro is None:
        return {}

    def entries(off):
        nn, ni = struct.unpack_from('<HH', f, off + 12)
        return [struct.unpack_from('<II', f, off + 16 + 8 * i) for i in range(nn + ni)]

    out = {}
    # root -> type -> id -> lang -> (rva, size)
    for typ, offv in entries(ro):
        if typ != 10:                       # 10 = RCDATA
            continue
        for rid, offv2 in entries(ro + (offv & 0x7fffffff)):
            langs = entries(ro + (offv2 & 0x7fffffff))
            if not langs:
                continue
            drva, dsz = struct.unpack_from('<II', f, ro + (langs[0][1] & 0x7fffffff))
            o = r2o(drva)
            if o is not None:
                out[rid] = (o, dsz)
    if out:
        return out
    # fall back: 2-level walk (some packs skip the language level)
    for rid, offv in entries(ro):
        lvl2 = entries(ro + (offv & 0x7fffffff))
        if not lvl2:
            continue
        drva, dsz = struct.unpack_from('<II', f, ro + (lvl2[0][1] & 0x7fffffff))
        o = r2o(drva)
        if o is not None:
            out[rid] = (o, dsz)
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        print(__doc__)
        return 2
    f = open(path, 'rb').read()
    print("file: %s (%d bytes)" % (path, len(f)))

    res = walk_resources(f)
    print("RCDATA resources: %d" % len(res))

    plans = []
    for rid, (o, sz) in sorted(res.items()):
        if sz < 64:
            continue
        head = f[o:o + 1]
        if head != b'{':
            continue
        try:
            j = json.loads(f[o:o + sz].decode('utf-8'))
        except Exception:
            continue
        plans.append((rid, sz, j))

    if not plans:
        print("no JSON resource found")
        return 1

    for rid, sz, j in plans:
        keys = list(j.keys())
        print("\n" + "=" * 74)
        print("resource %s  (%d bytes)  keys: %s" % (rid, sz, keys[:12]))
        if 'phases' not in j:
            continue

        print("  schema         : %s" % j.get('schema'))
        print("  geometry_abi   : %s" % j.get('geometry_abi'))
        if 'source_runtime' in j:
            print("  source_runtime : %s" % j.get('source_runtime'))
        if 'source_sha256' in j:
            print("  source_sha256  : %s" % j.get('source_sha256'))

        kinds = Counter()
        total = 0
        for ph, lst in j['phases'].items():
            print("  phase %-12s %d ops" % (ph, len(lst)))
            for it in lst:
                kinds[it.get('op')] += 1
                total += 1
        print("  total ops      : %d   kinds: %s" % (total, dict(kinds)))

        entries = sorted(set(it.get('entry') for lst in j['phases'].values()
                             for it in lst if it.get('entry')))
        print("  distinct entries (%d):" % len(entries))
        for e in entries:
            print("      %s" % e)

        bufs = j.get('buffers') or []
        print("  buffers        : %d" % len(bufs))
        if bufs and isinstance(bufs[0], dict):
            print("    buffer keys  : %s" % list(bufs[0].keys()))

        # ---- the part that matters: what does the model consume from outside?
        blob = json.dumps(j)
        print("\n  --- external input candidates (strings in the plan) ---")
        pats = ['mv', 'motion', 'depth', 'distortion', 'clip', 'hud', 'ui',
                'backbuffer', 'back_buffer', 'output', 'exposure', 'jitter',
                'matri', 'camera', 'prev', 'cur', 'reset', 'render', 'alpha',
                'bias', 'mask', 'normal', 'linear', 'depth_', 'in_', 'src']
        found = {}
        for p in pats:
            hits = set(re.findall(r'"([A-Za-z0-9_.]*%s[A-Za-z0-9_.]*)"' % p, blob, re.I))
            if hits:
                found[p] = sorted(hits)[:14]
        for p, hits in found.items():
            print("    %-12s %s" % (p, ", ".join(hits)))

        if '--buffers' in sys.argv:
            print("\n  --- buffer descriptors ---")
            for i, b in enumerate(bufs):
                print("    [%3d] %s" % (i, json.dumps(b)))

        if '--ops' in sys.argv:
            for ph, lst in j['phases'].items():
                print("\n  --- ops in phase %s ---" % ph)
                for it in lst[:40]:
                    print("    op=%-14s entry=%-30s grid=%s block=%s binds=%s" % (
                        it.get('op'), it.get('entry'), it.get('grid'),
                        it.get('block'), it.get('bindings')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
