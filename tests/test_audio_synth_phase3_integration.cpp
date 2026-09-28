// Phase 3 integration: sequencer gene-group mutation (seeded reproducibility,
// lock behavior), the generative conductor mappings (disabled no-op,
// PEAK-vs-HOME density, morph-walk bounds), and a render smoke test of the
// showcase preset.
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/generative_conductor.hpp"
#include "dve/audio/patch_genetics.hpp"
#include "dve/audio/synthesizer.hpp"

namespace {
using namespace dve::audio;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const SynthPreset& showcase_preset() {
    static const std::vector<SynthPreset> presets = SynthPreset::builtin_presets();
    for (const auto& preset : presets)
        if (preset.name == "Generative Attractor Pad") return preset;
    throw std::runtime_error("showcase preset not found");
}

void test_sequencer_gene_reproducibility() {
    const SynthPreset parent = SynthPreset::make_default();
    const SynthPreset m1 = mutate_preset(parent, 0.5F, 12345ULL, 0U);
    const SynthPreset m2 = mutate_preset(parent, 0.5F, 12345ULL, 0U);
    require(m1.serialize() == m2.serialize(), "same seed gave different mutations");
    const SynthPreset m3 = mutate_preset(parent, 0.5F, 99999ULL, 0U);
    require(m1.serialize() != m3.serialize(), "different seeds gave identical mutations");
    require(m1.sequencer == m2.sequencer, "sequencer gene not reproducible");
    require(!(m1.sequencer == parent.sequencer), "sequencer gene left config unchanged");
    std::string error;
    require(m1.validate(&error), ("mutated preset invalid: " + error).c_str());
    std::cout << "sequencer gene reproducibility: OK\n";
}

void test_sequencer_gene_lock() {
    const SynthPreset parent = SynthPreset::make_default();
    const std::uint8_t locks = gene_group_bit(GeneGroup::Sequencer);
    const SynthPreset m = mutate_preset(parent, 1.0F, 4242ULL, locks);
    require(m.sequencer == parent.sequencer, "locked sequencer gene was mutated");
    // And with nothing locked the sequencer config must change at full intensity.
    const SynthPreset m2 = mutate_preset(parent, 1.0F, 4242ULL, 0U);
    require(!(m2.sequencer == parent.sequencer), "unlocked sequencer gene did not change");
    std::cout << "sequencer gene lock: OK\n";
}

void test_conductor_disabled_noop() {
    Synthesizer synth(48000);
    const float before = synth.sequencer().probability_scale();
    const auto scaleBefore = synth.sequencer().scale();
    const std::uint8_t rootBefore = synth.sequencer().root_note();
    synth.generative_conductor().process(synth, 0.01);  // never enabled
    require(synth.sequencer().probability_scale() == before, "disabled conductor moved density");
    require(synth.sequencer().scale() == scaleBefore, "disabled conductor moved scale");
    require(synth.sequencer().root_note() == rootBefore, "disabled conductor moved root");
    require(!synth.generative_conductor_enabled(), "conductor should default to disabled");
    std::cout << "conductor disabled no-op: OK\n";
}

void test_conductor_peak_vs_home() {
    Synthesizer synth(48000);
    AttractorConfig config;  // 4/4/4/2/4 bars @ 120 BPM, densities 0.15..0.95
    auto& conductor = synth.generative_conductor();
    conductor.configure(true, config);
    conductor.set_enabled(true);
    conductor.process(synth, 0.0);  // HOME, no time advanced
    const float homeScale = synth.sequencer().probability_scale();
    // At cycle start the state lerps from the FALL anchor (density 0.35).
    require(homeScale == 0.15F + 0.85F * 0.35F, "unexpected HOME density mapping");
    // Advance 12.5 bars (25 s at 120 BPM) into PEAK and re-sample.
    conductor.process(synth, 25.0);
    const float peakScale = synth.sequencer().probability_scale();
    require(peakScale > homeScale, "PEAK density scale not above HOME");
    require(peakScale <= 1.0F, "density scale out of range");
    std::cout << "conductor PEAK(" << peakScale << ") > HOME(" << homeScale << "): OK\n";
}

void test_conductor_morph_walk_bounds() {
    Synthesizer synth(48000);
    auto preset = showcase_preset();
    synth.set_preset(preset);
    auto& conductor = synth.generative_conductor();
    conductor.set_enabled(true);  // configure() already ran via set_preset
    // 200 blocks of wander: morphAmount must stay inside the state's region.
    for (int i = 0; i < 200; ++i) conductor.process(synth, 0.01);
    const float walked = synth.preset().morphAmount;
    require(walked >= 0.0F && walked <= 0.25F, "morph walk escaped its region");
    std::cout << "conductor morph walk bounds: OK (landed at " << walked << ")\n";
}

void test_showcase_renders_clean() {
    Synthesizer synth(48000);
    synth.set_preset(showcase_preset());
    require(synth.note_on(69, 0.9F), "note-on failed");
    std::vector<float> audio(48000 * 2 * 2, 0.0F);  // 2 seconds, stereo
    synth.render(audio);
    float peak = 0.0F;
    float maxDiff = 0.0F;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        require(std::isfinite(audio[i]), "non-finite sample rendered");
        peak = std::max(peak, std::abs(audio[i]));
        if (i > 0) maxDiff = std::max(maxDiff, std::abs(audio[i] - audio[i - 1]));
    }
    require(peak > 0.01F, "showcase rendered (near) silence");
    require(peak <= 1.0F, "showcase clipped past 0 dBFS");
    require(maxDiff < 0.3F, "showcase render has a discontinuity (click)");
    std::cout << "showcase render: peak=" << peak << " maxDiff=" << maxDiff << " OK\n";
}

}  // namespace

int main() {
    try {
        test_sequencer_gene_reproducibility();
        test_sequencer_gene_lock();
        test_conductor_disabled_noop();
        test_conductor_peak_vs_home();
        test_conductor_morph_walk_bounds();
        test_showcase_renders_clean();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "phase3 integration: all tests passed\n";
    return 0;
}
