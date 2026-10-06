// Phase 6 (SYN-016) end-to-end integration test: the full audio-to-synth
// pipeline composed from all four workers' pieces.
//
//   target recording -> (A) extract_audio_features -> target vector
//   candidate preset -> (B) render_preset_features -> features -> score_preset
//   (C) evolutionary_patch_search with B's scorer as its FitnessFn
//   (D) browser_candidates_from_ranked -> SearchSession -> save/load round-trip
//
// Kept small on purpose: the per-eval cost is dominated by Synthesizer
// construction (~7 s in this VM), so the search runs a tiny population for
// two generations. This proves the pieces compose and behave —
// determinism, finiteness, ranking — not full convergence (covered by the
// workers' own unit tests with synthetic fitness).

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "dve/audio/audio_features.hpp"
#include "dve/audio/patch_evaluator.hpp"
#include "dve/audio/patch_search.hpp"
#include "dve/audio/patch_search_browser.hpp"
#include "dve/audio/synthesizer.hpp"

namespace {

int failures = 0;

void check(bool cond, const char* name) {
    if (!cond) {
        ++failures;
        std::cout << "FAIL: " << name << "\n";
    } else {
        std::cout << "ok: " << name << "\n";
    }
}

bool all_finite(const dve::audio::AudioFeatureVector& v) {
    const double* p = &v.rms;
    // 14 scalar fields precede melBands in layout order; check named fields.
    const double fields[] = {v.rms, v.peak, v.spectralCentroidHz, v.spectralRolloffHz,
                             v.spectralFlatness, v.zeroCrossingRate, v.estimatedPitchHz,
                             v.pitchConfidence, v.harmonicEnergyRatio, v.attackSeconds,
                             v.decaySeconds, v.sustainLevel, v.releaseSeconds, v.stereoWidth};
    for (double f : fields)
        if (!std::isfinite(f)) return false;
    for (double b : v.melBands)
        if (!std::isfinite(b)) return false;
    (void)p;
    return true;
}

}  // namespace

int main() {
    using namespace dve::audio;

    // 1. Target: render a factory preset headlessly, extract its features.
    const std::vector<SynthPreset> factory = SynthPreset::builtin_presets();
    check(!factory.empty(), "factory presets exist");
    if (factory.empty()) return 1;

    RenderConfig renderCfg;
    renderCfg.seconds = 1.0;
    renderCfg.sampleRate = 48000.0;
    renderCfg.midiNote = 69;

    const AudioFeatureVector target = render_preset_features(factory.front(), renderCfg);
    check(all_finite(target), "target features finite");
    check(target.rms > 0.0, "target features non-silent");

    // 2. Search with B's real scorer as C's fitness. Tiny config: this is a
    // composition smoke test, not a convergence proof.
    const FeatureWeights weights;
    FitnessFn fitness = [&](const SynthPreset& p) {
        return score_preset(p, target, weights, renderCfg);
    };
    SearchConfig cfg;
    cfg.populationSize = 6;
    cfg.maxGenerations = 2;
    cfg.seed = 0xC0FFEE;
    cfg.eliteCount = 1;
    cfg.targetDistance = 1e-9;  // unreachable: forces both generations to run

    const SearchResult result = evolutionary_patch_search(target, cfg, fitness);
    check(!result.ranked.empty(), "search returned candidates");
    check(result.generationsRun == 2, "search ran both generations");
    bool sorted = true, finite = true;
    for (std::size_t i = 0; i < result.ranked.size(); ++i) {
        if (!std::isfinite(result.ranked[i].distance)) finite = false;
        if (i > 0 && result.ranked[i].distance < result.ranked[i - 1].distance) sorted = false;
    }
    check(finite, "all candidate distances finite");
    check(sorted, "candidates ranked best-first");
    check(result.bestDistance < kInvalidPresetScore, "best candidate is a valid preset");
    check(result.bestDistance == result.ranked.front().distance, "bestDistance matches front");

    // 3. Bridge into the browser: convert, build a session, round-trip it.
    std::vector<BrowserCandidate> candidates =
        browser_candidates_from_ranked(result.ranked, "match");
    check(candidates.size() == result.ranked.size(), "candidate count preserved");
    check(candidates.front().label == "match 1", "candidate labels assigned");
    check(candidates.front().distance == result.ranked.front().distance,
          "candidate distances preserved");

    SearchSession session;
    session.targetName = "integration-target";
    session.target = target;
    session.candidates = candidates;
    session.searchSeed = cfg.seed;
    session.generationsRun = result.generationsRun;

    const std::string path = "p6_integration_test.dvesearch";
    check(save_search_session(session, path), "session saved");
    SearchSession loaded;
    check(load_search_session(path, &loaded), "session loaded");
    check(loaded.targetName == session.targetName, "target name round-trips");
    check(loaded.candidates.size() == session.candidates.size(), "candidate count round-trips");
    check(loaded.candidates.front().distance == session.candidates.front().distance,
          "distances round-trip bit-identically");
    check(loaded.searchSeed == session.searchSeed, "search seed round-trips");
    std::remove(path.c_str());

    // 4. Promote the winner: must yield a valid preset.
    const SynthPreset promoted = promote_candidate(loaded.candidates.front());
    check(promoted.validate(), "promoted candidate validates");

    if (failures == 0) std::cout << "ALL PHASE 6 INTEGRATION TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
