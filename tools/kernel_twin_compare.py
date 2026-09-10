# ---------------------------------------------------------------------------
#  Can the FP8 kernel be swapped for its FP16 twin?
#
#  The plan under consideration is: let the host keep believing it is on an
#  FP8-capable device, then redirect every `conv1_fp8` lookup to `conv1` so the
#  FP16 twin runs instead.  That only works if the two share a *launch
#  interface*: same parameter block, same required block dimensions, same
#  scratch expectations.  If they do not, the failure is silent corruption
#  rather than an error, which is the expensive kind.
#
#  Two independent fingerprints, both read straight from the embedded images:
#    1. the set of constant-bank offsets the kernel dereferences.  Kernel
#       parameters live in constant bank 0 starting at a fixed base, so the set
#       of c[0x0][...] offsets touched by the prologue IS the parameter layout.
#    2. register count and shared-memory size, via cuobjdump -res-usage.
#  If both agree across a twin pair, a swap is plausible.  If either differs,
#  the host's FP8 launch configuration would be driving an FP16 kernel that
#  expects something else.
# ---------------------------------------------------------------------------
import os
import re
import subprocess
import sys
from collections import Counter

CUDA = r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin'
NVDISASM = os.path.join(CUDA, 'nvdisasm.exe')
CUOBJDUMP = os.path.join(CUDA, 'cuobjdump.exe')
SCRATCH = r'C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\build\sass_scratch'

# parameters live in constant bank 0; matches c[0x0][0x210], c[0x0][0x218], ...
CB0 = re.compile(r'c\[0x0\]\[(0x[0-9a-f]+)\]')
FUNC = re.compile(r'Function\s*:\s*([A-Za-z0-9_]+)')
RES = re.compile(r'Used\s+(\d+)\s+registers.*?(\d+)\s+bytes\s+smem', re.S)


def sass(path):
    r = subprocess.run([NVDISASM, '-c', path], capture_output=True, text=True,
                       errors='replace', timeout=180)
    if r.returncode != 0:
        r = subprocess.run([NVDISASM, path], capture_output=True, text=True,
                           errors='replace', timeout=180)
    return r.stdout


def res_usage(path):
    # cuobjdump -res-usage prints, per function:
    #   Function conv1:
    #     REG:72 STACK:0 SHARED:1600 LOCAL:0 CONSTANT[0]:464 ...
    # CONSTANT[0] is the size of constant bank 0, i.e. the built-in region plus
    # the kernel parameter block.  Equal CONSTANT[0] across a twin pair means an
    # equal parameter block, which is what "same launch interface" requires.
    # SHARED has to match too: that is the scratch the host has to provide.
    r = subprocess.run([CUOBJDUMP, '-res-usage', path], capture_output=True,
                       text=True, errors='replace', timeout=180)
    m = re.search(r'REG:(\d+)\s+STACK:(\d+)\s+SHARED:(\d+)\s+LOCAL:(\d+)\s+'
                  r'CONSTANT\[0\]:(\d+)', r.stdout)
    if not m:
        return None
    return dict(regs=int(m.group(1)), stack=int(m.group(2)),
                shared=int(m.group(3)), local=int(m.group(4)),
                cmem0=int(m.group(5)))


def profile(tag):
    path = os.path.join(SCRATCH, 'sm_89_%s.cubin' % tag)
    if not os.path.exists(path):
        return None
    txt = sass(path)
    offs = Counter(v for v in CB0.findall(txt))
    ints = sorted(int(v, 16) for v in offs)
    return {
        'tag': tag,
        'size': os.path.getsize(path),
        'funcs': sorted(set(FUNC.findall(txt))),
        'n_cb0_slots': len(offs),
        'cb0_min': min(ints) if ints else None,
        'cb0_max': max(ints) if ints else None,
        'cb0_set': set(ints),
        'n_inst': sum(1 for l in txt.splitlines() if '/*' in l and '*/' in l),
        'res': res_usage(path),
    }


PAIRS = [('conv1', 'conv1_fp8'), ('conv2', 'conv2_fp8'), ('attn1', 'attn1_fp8'),
         ('attn2', 'attn2_fp8'), ('conv_out3', 'conv_out3_fp8'),
         ('conv_fused', 'conv_fused_fp8'), ('depth_to_space', 'depth_to_space_fp8'),
         ('conv_proj1', 'conv_proj1_fp8')]

print('=' * 92)
print('FP16 twin vs FP8 twin  -  is the launch interface compatible?')
print('=' * 92)
print()
print('%-18s %8s  %-11s %-11s %-7s %-7s %-7s  %s'
      % ('kernel', 'size', 'cb0 range', 'cmem[0]', 'regs', 'smem', 'inst', 'verdict'))
print('-' * 92)

same_layout = 0
diff_layout = 0
for a, b in PAIRS:
    pa, pb = profile(a), profile(b)
    if not pa or not pb:
        print('  %-16s missing' % a)
        continue
    for p in (pa, pb):
        rng = ('%#x..%#x' % (p['cb0_min'], p['cb0_max'])) if p['cb0_min'] is not None else '?'
        rr = p['res'] or {}
        regs, smem, cmem0 = rr.get('regs'), rr.get('shared'), rr.get('cmem0')
        print('%-18s %8d  %-11s %-11s %-7s %-7s %-7d'
              % (p['tag'], p['size'], rng,
                 ('%d' % cmem0) if cmem0 is not None else '?',
                 regs if regs is not None else '?',
                 smem if smem is not None else '?', p['n_inst']))
    ra, rb = pa['res'], pb['res']
    if ra and rb:
        param_same = (ra['cmem0'] == rb['cmem0'])
        shared_same = (ra['shared'] == rb['shared'])
        ok = ('params SAME (%d B)' % ra['cmem0']) if param_same else \
             ('params DIFFER (%d vs %d B)' % (ra['cmem0'], rb['cmem0']))
        print('%-18s %8s  params %-6s shared %-6s regs %-8s -> %s'
              % ('', '',
                 '%d/%d' % (ra['cmem0'], rb['cmem0']),
                 '%d/%d%s' % (ra['shared'], rb['shared'],
                              '' if shared_same else ' DIFF'),
                 '%d/%d' % (ra['regs'], rb['regs']),
                 'COMPATIBLE' if (param_same and shared_same) else 'NOT COMPATIBLE'))
    else:
        param_same = None
        shared_same = None
        ok = 'unknown'
        print('%-18s %8s  (no resource usage)' % ('', ''))
    if param_same and shared_same:
        same_layout += 1
    elif param_same is False or shared_same is False:
        diff_layout += 1
    print('-' * 92)

print()
print('summary: %d pair(s) compatible, %d not, %d unknown'
      % (same_layout, diff_layout, len(PAIRS) - same_layout - diff_layout))
print()
print('A pair is COMPATIBLE when the parameter block (CONSTANT[0]) and the')
print('shared-memory scratch both match, because those are the two things the')
print('host supplies at launch.  Register count may differ freely - it is chosen')
print('by ptxas and does not affect the launch contract.')
print()
if diff_layout == 0 and same_layout:
    print('VERDICT: every twin pair shares a launch interface, so redirecting an')
    print('_fp8 lookup to its FP16 twin at cuModuleGetFunction is plausible.')
    print('It still has to be confirmed by running one kernel under the host FP8')
    print('launch configuration and checking the numbers.')
elif diff_layout:
    print('VERDICT: at least one pair does not share a launch interface.')
    print('Substituting there would drive the FP16 kernel with the host FP8')
    print('arguments - silent wrong results rather than an error.')
