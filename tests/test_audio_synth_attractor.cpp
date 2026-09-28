// Tests for the attractor sequencer prototype (Phase 3).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dve/audio/attractor.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static AttractorConfig test_config() {
    AttractorConfig c;
    c.bpm = 60.0;  // 1 bar = 4 seconds
    c.barsHome = 2.0;
    c.barsRise = 2.0;
    c.barsTension = 2.0;
    c.barsPeak = 1.0;
    c.barsFall = 2.0;
    c.seed = 12345ULL;
    return c;
}

int main() {
    // Test 1: Scale factories.
    {
        Scale p = Scale::pentatonic(0);
        CHECK(p.intervals == (std::vector<int>{0, 2, 4, 7, 9}));
        CHECK(Scale::chromatic(5).pitchClassCount() == 12);
        CHECK(Scale::major(0).intervals[0] == 0);
        CHECK(Scale::dorian(3).rootSemitone == 3);
        CHECK(Scale::phrygian(0).name == "phrygian");
    }

    // Test 2: full cycle in order HOME->RISE->TENSION->PEAK->FALL->HOME.
    {
        AttractorSequencer seq(test_config());
        CHECK(seq.phase() == AttractorPhase::Home);
        const std::vector<AttractorPhase> expected = {
            AttractorPhase::Rise, AttractorPhase::Tension, AttractorPhase::Peak,
            AttractorPhase::Fall, AttractorPhase::Home,
        };
        const std::vector<double> durs = {2.0, 2.0, 2.0, 1.0, 2.0};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            AttractorState s = seq.advance_bars(durs[i]);
            CHECK(s.phase == expected[i]);
            CHECK(seq.phase() == expected[i]);
            CHECK(s.phaseProgress01 < 1e-6F);
        }
        // Cycle continues into the next loop.
        AttractorState s = seq.advance_bars(2.0);
        CHECK(s.phase == AttractorPhase::Rise);
        CHECK(std::abs(seq.bars_elapsed() - 11.0) < 1e-9);
    }

    // Test 3: bar timing via seconds at a given BPM.
    {
        AttractorSequencer seq(test_config());  // 60 BPM: 1 bar = 4 s, HOME = 2 bars
        seq.advance(7.99);
        CHECK(seq.phase() == AttractorPhase::Home);
        seq.advance(0.01);  // exactly 8.0 s total
        CHECK(seq.phase() == AttractorPhase::Rise);
        seq.advance(8.0);  // 2 more bars: RISE done -> TENSION entered
        CHECK(seq.phase() == AttractorPhase::Tension);
        CHECK(std::abs(seq.phase_progress()) < 1e-9);
        seq.advance(4.0);  // 1 bar into the 2-bar TENSION phase
        CHECK(std::abs(seq.phase_progress() - 0.5) < 1e-9);
    }

    // Test 4: density/morph progression toward PEAK and back.
    {
        AttractorSequencer seq(test_config());
        seq.advance_bars(2.0);  // end of HOME
        float homeD = seq.current().rhythmDensity01;
        seq.advance_bars(2.0);  // end of RISE
        float riseD = seq.current().rhythmDensity01;
        seq.advance_bars(2.0);  // end of TENSION
        float tensionD = seq.current().rhythmDensity01;
        AttractorState peak = seq.advance_bars(1.0);  // end of PEAK
        float peakD = peak.rhythmDensity01;
        seq.advance_bars(2.0);  // end of FALL
        float fallD = seq.current().rhythmDensity01;
        CHECK(homeD < riseD);
        CHECK(riseD < tensionD);
        CHECK(tensionD < peakD);
        CHECK(fallD < peakD);
        CHECK(peakD > homeD);
        CHECK(std::abs(peakD - 0.95F) < 1e-6F);
        CHECK(std::abs(homeD - 0.15F) < 1e-6F);
        // PEAK allows the full morph sweep; HOME sits near A.
        CHECK(std::abs(peak.morphMin - 0.0F) < 1e-6F);
        CHECK(std::abs(peak.morphMax - 1.0F) < 1e-6F);
        AttractorSequencer seq2(test_config());
        seq2.advance_bars(2.0);
        CHECK(std::abs(seq2.current().morphMax - 0.25F) < 1e-6F);
        CHECK(seq2.current().mutationIntensity01 < peak.mutationIntensity01);
        CHECK(std::abs(peak.mutationIntensity01 - 1.0F) < 1e-6F);
    }

    // Test 5: intra-state ramps are continuous (mid-state values between start/end).
    {
        AttractorSequencer seq(test_config());
        // Mid-RISE: ramps from HOME anchor (0.15) to RISE anchor (0.38).
        seq.advance_bars(2.0);  // end of HOME
        seq.advance_bars(1.0);  // halfway through RISE
        AttractorState mid = seq.current();
        CHECK(mid.phase == AttractorPhase::Rise);
        CHECK(mid.rhythmDensity01 > 0.15F && mid.rhythmDensity01 < 0.38F);
        CHECK(mid.morphMax > 0.25F && mid.morphMax < 0.50F);
        CHECK(mid.mutationIntensity01 > 0.10F && mid.mutationIntensity01 < 0.30F);
        // Finer sampling is monotone within the state.
        float prevD = 0.15F;
        bool monotone = true;
        for (int i = 1; i <= 10; ++i) {
            AttractorSequencer s2(test_config());
            s2.advance_bars(2.0 + 2.0 * i / 10.0);
            float d = s2.current().rhythmDensity01;
            if (d < prevD - 1e-6F) monotone = false;
            prevD = d;
        }
        CHECK(monotone);
        // No jump at a boundary: last sample of RISE vs first of TENSION.
        AttractorSequencer a(test_config());
        a.advance_bars(3.999);
        AttractorSequencer b(test_config());
        b.advance_bars(4.001);
        CHECK(std::abs(a.current().rhythmDensity01 - b.current().rhythmDensity01) < 0.01F);
        CHECK(std::abs(a.current().morphMax - b.current().morphMax) < 0.01F);
    }

    // Test 6: seeded reproducibility (same seed, any chunking -> same scales).
    {
        AttractorSequencer a(test_config());
        AttractorSequencer b(test_config());
        std::vector<std::string> scalesA, scalesB;
        for (int i = 0; i < 5; ++i) {
            scalesA.push_back(a.current().scale.name + std::to_string(a.current().scale.rootSemitone));
            a.advance_bars(9.0);  // whole cycle in one call
        }
        for (int i = 0; i < 5; ++i) {
            scalesB.push_back(b.current().scale.name + std::to_string(b.current().scale.rootSemitone));
            b.advance_bars(3.0);
            b.advance_bars(3.0);
            b.advance_bars(3.0);  // same 9 bars, chunked differently
        }
        CHECK(scalesA == scalesB);
        CHECK(scalesA[0].substr(0, 10) == "pentatonic");  // HOME always pentatonic
        // Different seed still cycles phases in order (no crash, sane states).
        AttractorConfig other = test_config();
        other.seed = 999ULL;
        AttractorSequencer c(other);
        c.advance_bars(9.0);
        CHECK(c.phase() == AttractorPhase::Home);
    }

    // Test 7: reset() returns to HOME and restarts the draw sequence.
    {
        AttractorSequencer seq(test_config());
        seq.advance_bars(6.5);  // into PEAK
        CHECK(seq.phase() == AttractorPhase::Peak);
        std::string peakScale = seq.current().scale.name;
        (void)peakScale;
        seq.reset();
        CHECK(seq.phase() == AttractorPhase::Home);
        CHECK(seq.bars_elapsed() == 0.0);
        CHECK(seq.phase_progress() == 0.0);
        CHECK(seq.current().phaseProgress01 == 0.0F);
        AttractorSequencer fresh(test_config());
        CHECK(seq.current().scale == fresh.current().scale);
        seq.advance_bars(4.0);
        fresh.advance_bars(4.0);
        CHECK(seq.current().scale == fresh.current().scale);
    }

    // Test 8: zero-duration phases are skipped instantly; multi-boundary advance.
    {
        AttractorConfig c = test_config();
        c.barsPeak = 0.0;
        AttractorSequencer seq(c);
        seq.advance_bars(6.0);  // HOME(2)+RISE(2)+TENSION(2), PEAK skipped -> FALL
        CHECK(seq.phase() == AttractorPhase::Fall);
        seq.advance_bars(2.0);
        CHECK(seq.phase() == AttractorPhase::Home);
        // Negative / zero advance is ignored.
        AttractorSequencer seq2(test_config());
        seq2.advance_bars(-3.0);
        seq2.advance(-1.0);
        CHECK(seq2.phase() == AttractorPhase::Home);
        CHECK(seq2.bars_elapsed() == 0.0);
    }

    if (g_failures == 0) std::printf("attractor: all tests passed\n");
    return g_failures;
}
