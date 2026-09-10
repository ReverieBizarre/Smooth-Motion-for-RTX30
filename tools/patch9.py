# ---------------------------------------------------------------------------
#  Dump the raw flow field so it can be analysed against the analytic field
#  offline.  The existing PGM dump is normalised to 8 bits for eyeballing,
#  which is useless for measuring a 1 px error.  Raw RG32F float2 costs
#  8 bytes/px and answers "where exactly is the matcher wrong".
# ---------------------------------------------------------------------------
import io

p = 'tools/selftest.cpp'
s = io.open(p, encoding='utf-8').read()


def sub(old, new, cnt=1):
    global s
    assert s.count(old) == cnt, ('anchor miss', old[:70], s.count(old))
    s = s.replace(old, new)


sub('''        if (visual && outDir)
        {
            float mx = 0;''',
    '''        // raw float dump, unconditional: the analysis script needs the real
        // values, and 8-bit normalised output cannot resolve a 1 px error
        if (outDir)
        {
            char pb[512];
            sprintf_s(pb, "%s/%dx%d_flowAB.f32", outDir, W, H);
            FILE* f = fopen(pb, "wb");
            if (f)
            {
                std::vector<float> tmp((size_t)W * H * 2);
                for (int y = 0; y < H; ++y)
                    memcpy(tmp.data() + (size_t)y * W * 2,
                           fl.data.data() + (size_t)y * fl.rowPitch, (size_t)W * 8);
                fwrite(tmp.data(), 4, tmp.size(), f);
                fclose(f);
            }
            Raw fb = readBackRaw(g, eng.debug_flow_ba(), eng.debug_state(eng.debug_flow_ba()));
            sprintf_s(pb, "%s/%dx%d_flowBA.f32", outDir, W, H);
            f = fopen(pb, "wb");
            if (f)
            {
                std::vector<float> tmp((size_t)W * H * 2);
                for (int y = 0; y < H; ++y)
                    memcpy(tmp.data() + (size_t)y * W * 2,
                           fb.data.data() + (size_t)y * fb.rowPitch, (size_t)W * 8);
                fwrite(tmp.data(), 4, tmp.size(), f);
                fclose(f);
            }
            Raw cs = readBackRaw(g, eng.debug_cost(), eng.debug_state(eng.debug_cost()));
            sprintf_s(pb, "%s/%dx%d_cost.f32", outDir, W, H);
            f = fopen(pb, "wb");
            if (f)
            {
                std::vector<float> tmp((size_t)W * H);
                for (int y = 0; y < H; ++y)
                    memcpy(tmp.data() + (size_t)y * W,
                           cs.data.data() + (size_t)y * cs.rowPitch, (size_t)W * 4);
                fwrite(tmp.data(), 4, tmp.size(), f);
                fclose(f);
            }
        }

        if (visual && outDir)
        {
            float mx = 0;''')

# 1080p and 1440p need the dumps too
sub('''    if (runSize(g, opt, shader, 1920, 1080, false, outDir, 200)) return 1;
    if (runSize(g, opt, shader, 2560, 1440, false, outDir, 150)) return 1;''',
    '''    if (runSize(g, opt, shader, 1920, 1080, false, outDir, 200)) return 1;
    if (runSize(g, opt, shader, 2560, 1440, false, outDir, 60)) return 1;''')

io.open(p, 'w', encoding='utf-8').write(s)
print('selftest.cpp patched (raw flow dump)')
