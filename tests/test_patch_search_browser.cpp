// Phase 6 (SYN-016) candidate browser tests: session persistence round-trip,
// forward compatibility, corruption handling, ranking, promotion, and the
// editor Search panel wiring.
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

#include "dve/audio/patch_search_browser.hpp"

namespace {

namespace audio = dve::audio;
namespace editor = dve::editor;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool presets_equal(const audio::SynthPreset& a, const audio::SynthPreset& b) {
    return a.serialize() == b.serialize();
}

bool targets_equal(const audio::AudioFeatureVector& a, const audio::AudioFeatureVector& b) {
    // Bit-exact: the session format round-trips doubles losslessly.
    const double* pa = &a.rms;
    const double* pb = &b.rms;
    for (int i = 0; i < 14; ++i)
        if (pa[i] != pb[i]) return false;
    for (int i = 0; i < 16; ++i)
        if (a.melBands[i] != b.melBands[i]) return false;
    return true;
}

// A valid preset with deterministic pseudo-random musical variation.
audio::SynthPreset random_preset(std::mt19937& rng, int index) {
    audio::SynthPreset preset = audio::SynthPreset::make_default();
    preset.name = "SearchCand_" + std::to_string(index);
    std::uniform_real_distribution<float> gain(0.1F, 0.9F);
    std::uniform_real_distribution<float> cutoff(200.0F, 8000.0F);
    std::uniform_int_distribution<int> wave(0, 5);
    preset.masterGain = gain(rng);
    preset.filter.cutoffHertz = cutoff(rng);
    preset.oscillators[0].waveform =
        static_cast<audio::OscillatorWaveform>(wave(rng));
    preset.oscillators[1].enabled = (index % 2) == 0;
    std::string error;
    require(preset.validate(&error), ("random preset invalid: " + error).c_str());
    return preset;
}

audio::SearchSession make_session() {
    std::mt19937 rng(1234);
    audio::SearchSession session;
    session.targetName = "Glass Bell Target";
    session.searchSeed = 987654321ULL;
    session.generationsRun = 12;
    // Fill the target with non-trivial finite values incl. negatives.
    double* fields = &session.target.rms;
    std::uniform_real_distribution<double> dist(-2.5, 44000.0);
    for (int i = 0; i < 14; ++i) fields[i] = dist(rng);
    for (int i = 0; i < 16; ++i) session.target.melBands[i] = dist(rng) * 0.001;
    const double distances[5] = {0.42, 0.07, 1.25, 0.33, 0.07};  // note the tie
    for (int i = 0; i < 5; ++i) {
        audio::BrowserCandidate candidate;
        candidate.preset = random_preset(rng, i);
        candidate.distance = distances[i];
        candidate.label = "gen12-" + std::to_string(100 + i);
        session.candidates.push_back(std::move(candidate));
    }
    audio::sort_candidates(session);  // best-first: 0.07, 0.07, 0.33, 0.42, 1.25
    return session;
}

void click_rect(editor::PatchSearchBrowserPanel& panel, editor::UiRect rect,
                audio::Synthesizer& synth) {
    const int cx = rect.x + rect.width / 2;
    const int cy = rect.y + rect.height / 2;
    require(panel.pointer_down(cx, cy, synth), "click was not consumed by the panel");
    panel.pointer_up(cx, cy, synth);
}

}  // namespace

int main() {
    try {
        const std::filesystem::path tmpDir =
            std::filesystem::temp_directory_path() / "dve_p6d_browser_tests";
        std::error_code ec;
        std::filesystem::remove_all(tmpDir, ec);
        std::filesystem::create_directories(tmpDir, ec);

        // 1. Session round-trip: 5 candidates, distinct distances (with a tie).
        {
            const audio::SearchSession session = make_session();
            const std::string path = (tmpDir / "session.dvesearch").string();
            require(audio::save_search_session(session, path), "save_search_session failed");
            audio::SearchSession loaded;
            require(audio::load_search_session(path, &loaded), "load_search_session failed");
            require(loaded.targetName == session.targetName, "targetName mismatch");
            require(loaded.searchSeed == session.searchSeed, "searchSeed mismatch");
            require(loaded.generationsRun == session.generationsRun, "generationsRun mismatch");
            require(targets_equal(loaded.target, session.target), "target vector mismatch");
            require(loaded.candidates.size() == session.candidates.size(), "candidate count mismatch");
            for (std::size_t i = 0; i < session.candidates.size(); ++i) {
                require(loaded.candidates[i].label == session.candidates[i].label,
                        "candidate label/order mismatch");
                require(loaded.candidates[i].distance == session.candidates[i].distance,
                        "candidate distance not bit-identical");
                require(presets_equal(loaded.candidates[i].preset, session.candidates[i].preset),
                        "candidate preset mismatch after round-trip");
                std::string error;
                require(loaded.candidates[i].preset.validate(&error), "loaded preset invalid");
            }
            // Stable order for the tied distances (insertion order kept).
            require(loaded.candidates[0].label == "gen12-101", "tie order not stable");
            require(loaded.candidates[1].label == "gen12-104", "tie order not stable");
            std::cout << "round-trip: OK\n";
        }

        // 2. Forward compatibility: unknown fields are skipped.
        {
            const std::string path = (tmpDir / "future.dvesearch").string();
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out << "# hand-written future-format session\n"
                       "dve_search_session=1\n"
                       "target_name=Future Bell\n"
                       "search_seed=42\n"
                       "generations_run=3\n"
                       "target.rms=0.5\n"
                       "target.some_new_metric=0.25\n"
                       "neural_proposal_confidence=0.9\n"
                       "candidate_count=1\n"
                       "[candidate]\n"
                       "label=gen0-1\n"
                       "distance=0.125\n"
                       "neural_score=0.77\n"
                       "[dve_search_preset_begin]\n"
                       "DVE_SYNTH_PRESET=5\n"
                       "name=FuturePreset\n"
                       "[dve_search_preset_end]\n";
            }
            audio::SearchSession loaded;
            require(audio::load_search_session(path, &loaded), "future-format load failed");
            require(loaded.targetName == "Future Bell", "known field lost");
            require(loaded.searchSeed == 42, "known field lost");
            require(loaded.target.rms == 0.5, "known target field lost");
            require(loaded.candidates.size() == 1, "candidate lost");
            require(loaded.candidates[0].label == "gen0-1", "candidate label lost");
            require(loaded.candidates[0].distance == 0.125, "candidate distance lost");
            require(loaded.candidates[0].preset.name == "FuturePreset", "preset lost");
            std::cout << "forward-compatibility: OK\n";
        }

        // 3. Corrupt file (truncated mid-preset) -> false, no crash.
        {
            const audio::SearchSession session = make_session();
            const std::string full = (tmpDir / "full.dvesearch").string();
            const std::string cut = (tmpDir / "cut.dvesearch").string();
            require(audio::save_search_session(session, full), "save failed");
            std::ifstream in(full, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            require(bytes.size() > 100, "session file unexpectedly small");
            {
                std::ofstream out(cut, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() * 3 / 5));
            }
            audio::SearchSession loaded;
            require(!audio::load_search_session(cut, &loaded), "truncated load should fail");
            // Garbage with no magic line also fails cleanly.
            const std::string junk = (tmpDir / "junk.dvesearch").string();
            {
                std::ofstream out(junk, std::ios::binary | std::ios::trunc);
                out << "this is not a session file\nkey=value\n";
            }
            require(!audio::load_search_session(junk, &loaded), "junk load should fail");
            std::cout << "corrupt-file: OK\n";
        }

        // 4. Missing file -> false.
        {
            audio::SearchSession loaded;
            require(!audio::load_search_session((tmpDir / "nope.dvesearch").string(), &loaded),
                    "missing file should fail");
            require(!audio::load_search_session((tmpDir / "nope.dvesearch").string(), nullptr),
                    "null session should fail");
            std::cout << "missing-file: OK\n";
        }

        // 5. keep_top_n keeps the best N in order; promote_candidate validates.
        {
            audio::SearchSession session = make_session();
            audio::keep_top_n(session, 3);
            require(session.candidates.size() == 3, "keep_top_n size wrong");
            require(session.candidates[0].distance == 0.07, "keep_top_n order wrong");
            require(session.candidates[1].distance == 0.07, "keep_top_n order wrong");
            require(session.candidates[2].distance == 0.33, "keep_top_n order wrong");
            const audio::SynthPreset promoted = audio::promote_candidate(session.candidates[0]);
            require(presets_equal(promoted, session.candidates[0].preset),
                    "promote changed a valid preset");
            std::string error;
            require(promoted.validate(&error), "promoted preset invalid");
            // Invalid candidate -> documented default-constructed fallback.
            audio::BrowserCandidate bad;
            bad.preset.name = "";  // empty name fails validation (1..128 chars)
            bad.preset.masterGain = 100.0F;
            require(!bad.preset.validate(nullptr), "test setup: preset should be invalid");
            const audio::SynthPreset fallback = audio::promote_candidate(bad);
            require(fallback.validate(&error), "promote fallback invalid");
            require(fallback.name == audio::SynthPreset::make_default().name,
                    "promote fallback is not the default preset");
            std::cout << "ranking+promote: OK\n";
        }

        // 6. Editor Search panel wiring.
        {
            audio::Synthesizer synth;
            editor::PatchSearchBrowserPanel panel;
            require(!panel.open(), "panel should start closed");
            panel.set_session(make_session());
            panel.set_open(true);
            panel.resize(100, 100, 420, 420);
            require(panel.open(), "panel did not open");

            // Select the second visible candidate by clicking its row.
            const auto rows = panel.layout().candidateRows;
            click_rect(panel, rows[1], synth);
            require(panel.selected_candidate() == 1, "candidate row click did not select");

            // Audition routes through the hook (override the PHASE6 STUB).
            bool hookCalled = false;
            std::string hookedName;
            panel.set_audition_hook(
                [&](audio::Synthesizer&, const audio::SynthPreset& preset) {
                    hookCalled = true;
                    hookedName = preset.name;
                });
            click_rect(panel, panel.layout().auditionButton, synth);
            require(hookCalled, "audition hook was not called");
            require(hookedName == panel.session().candidates[1].preset.name,
                    "audition hook got the wrong preset");

            // Default hook is the marked stub: makes the preset live.
            panel.set_audition_hook(audio::stub_audition_preset);
            click_rect(panel, panel.layout().auditionButton, synth);
            require(synth.preset().name == hookedName, "stub audition did not set live preset");

            // Promote saves a .dvesynth and makes it the live preset.
            const std::filesystem::path promoteDir = tmpDir / "promoted";
            panel.set_promote_directory(promoteDir);
            click_rect(panel, panel.layout().promoteButton, synth);
            const std::string status(panel.status());
            require(status.rfind("Promoted:", 0) == 0, ("promote status wrong: " + status).c_str());
            bool foundWav = false;
            for (const auto& entry : std::filesystem::directory_iterator(promoteDir, ec))
                if (entry.path().extension() == ".dvesynth") foundWav = true;
            require(foundWav, "promote did not write a .dvesynth file");
            require(synth.preset().name == hookedName, "promote did not set live preset");

            // Click outside the panel is not consumed.
            require(!panel.pointer_down(10, 10, synth), "outside click consumed");
            std::cout << "editor-panel: OK\n";
        }

        std::filesystem::remove_all(tmpDir, ec);
        std::cout << "ALL PATCH SEARCH BROWSER TESTS PASSED\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
