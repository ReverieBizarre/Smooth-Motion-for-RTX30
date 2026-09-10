# ---------------------------------------------------------------------------
#  Report the flow error *distribution*, not just the median.
#
#  A median is only meaningful for a unimodal population.  If the matcher
#  locks on correctly in most cells and collapses to ~0 in a minority, the
#  median lands on neither mode and reads like a uniform 13% underestimate -
#  which would send me off tuning the wrong thing.  Percentiles plus the
#  fraction-within-1px separate those two cases immediately.
# ---------------------------------------------------------------------------
import io

p = 'tools/selftest.cpp'
s = io.open(p, encoding='utf-8').read()


def sub(old, new, cnt=1):
    global s
    assert s.count(old) == cnt, ('anchor miss', old[:70], s.count(old))
    s = s.replace(old, new)


sub('''        printf("flow fAB.x in background       : %6.2f px   (analytic %6.2f)\\n",
               medianOf(xs), expectedShift);
        printf("flow fAB.y in background       : %6.2f px   (analytic   0.00)\\n", medianOf(ys));''',
    '''        printf("flow fAB.x in background       : %6.2f px   (analytic %6.2f)\\n",
               medianOf(xs), expectedShift);
        printf("flow fAB.y in background       : %6.2f px   (analytic   0.00)\\n", medianOf(ys));

        // error distribution: |fAB.x - analytic|, plus the fraction that is
        // within one pixel.  The latter is the number that actually predicts
        // the blend output - a field that is right on 95% of pixels and
        // garbage on 5% is not "5% wrong", it is a 5%-area artefact.
        std::vector<float> ax;
        ax.reserve(xs.size());
        double sumE = 0.0; size_t nGood = 0;
        for (size_t i = 0; i < xs.size(); ++i)
        {
            const float e = fabsf(xs[i] - expectedShift);
            ax.push_back(e);
            sumE += e;
            if (e <= 1.0f) ++nGood;
        }
        std::sort(ax.begin(), ax.end());
        auto pct = [&](double q) { return ax[(size_t)(q * (ax.size() - 1) + 0.5)]; };
        printf("flow |err| x: p50 %6.2f  p90 %6.2f  p99 %6.2f  max %6.2f px\\n",
               pct(0.50), pct(0.90), pct(0.99), ax.back());
        printf("flow mean |err|                : %6.2f px\\n", sumE / (double)xs.size());
        printf("flow within 1 px of analytic   : %6.1f %%   <- what the blend sees\\n",
               100.0 * (double)nGood / (double)xs.size());''')

io.open(p, 'w', encoding='utf-8').write(s)
print('selftest.cpp patched (flow error distribution)')
