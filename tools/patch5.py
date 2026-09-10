# ---------------------------------------------------------------------------
#  Wire the externally supplied flow field into the execution path.
#
#  set_flow_override() was declared but never consulted, so the oracle run
#  would have silently produced "estimate + blend" numbers again.  Three edits:
#    1. a flag, read once
#    2. the estimation half (luma pyramid + matcher) becomes conditional
#    3. the blend stage picks whichever flow source is active
#  Also: dispatch_count_ is now per-record instead of monotonic.
# ---------------------------------------------------------------------------
import io

p = 'src/vfi.cpp'
s = io.open(p, encoding='utf-8').read()


def sub(old, new, cnt=1):
    global s
    assert s.count(old) == cnt, ('anchor miss', old[:70], s.count(old))
    s = s.replace(old, new)


def indent(block, n=4):
    pad = ' ' * n
    return ''.join((pad + ln if ln.strip() else ln) for ln in block.splitlines(True))


# ---- 0. per-frame dispatch count ----
sub('''    if (!ready_ || !cl || !inA || !inB || !out) return false;
    if (slot >= kMaxSlots) return false;''',
    '''    if (!ready_ || !cl || !inA || !inB || !out) return false;
    if (slot >= kMaxSlots) return false;

    dispatch_count_ = 0;''')

# ---- 1. the flag ----
sub('''    auto groups = [&](uint32_t n) { return ceildiv(n, 16u); };''',
    '''    auto groups = [&](uint32_t n) { return ceildiv(n, 16u); };

    // -----------------------------------------------------------------
    // Oracle mode.  An externally supplied flow field replaces the entire
    // estimation half.  This exists to separate two questions that are
    // otherwise indistinguishable in the numbers: "my matcher is bad" and
    // "classical warp/blend has a ceiling of its own".  Only 5 dispatches
    // run in this mode (2 colour downsample + 2 gaussian + 1 blend), which
    // is also the floor for a runtime that gets its flow from the engine
    // instead of estimating it.
    // -----------------------------------------------------------------
    const bool oracle = has_flow_override();''')

# ---- 2. gate the estimation half ----
i0 = s.index('    // ---------------- luma ----------------\n')
i1 = s.index('    // ---------------- half-res colour base ----------------\n')
s = (s[:i0]
     + '    // ---- estimation half: luma pyramid + matcher (skipped in oracle mode) ----\n'
     + '    if (!oracle)\n    {\n'
     + indent(s[i0:i1])
     + '    }\n\n'
     + s[i1:])

i0 = s.index('    // ---------------- optical flow, both directions ----------------\n')
i1 = s.index('    // ---------------- synthesis ----------------\n')
s = (s[:i0]
     + '    // ---------------- optical flow, both directions (skipped in oracle mode) ----\n'
     + '    if (!oracle)\n    {\n'
     + indent(s[i0:i1])
     + '    }\n\n'
     + s[i1:])

# ---- 3. blend reads whichever flow is live ----
sub('''    go(psoBlend_, pc,
       { { 8, inA }, { 9, inB }, { 10, blurA_ }, { 11, blurB_ },
         { 12, flow_[0][4] }, { 13, flow_[1][4] }, { 14, costFull_[0] } },
       { { 6, out } }, groups(W), groups(H));''',
    '''    ID3D12Resource* fAB  = oracle ? ovrAB_   : flow_[0][4];
    ID3D12Resource* fBA  = oracle ? ovrBA_   : flow_[1][4];
    ID3D12Resource* fCst = oracle ? ovrCost_ : costFull_[0];

    go(psoBlend_, pc,
       { { 8, inA }, { 9, inB }, { 10, blurA_ }, { 11, blurB_ },
         { 12, fAB }, { 13, fBA }, { 14, fCst } },
       { { 6, out } }, groups(W), groups(H));''')

io.open(p, 'w', encoding='utf-8').write(s)
print('vfi.cpp patched')
