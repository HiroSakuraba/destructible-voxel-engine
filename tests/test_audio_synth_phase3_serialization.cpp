// Phase 3: serialization round-trip for the generative preset fields
// (sequencer config, genetics settings, attractor config) plus backward
// compatibility: preset text without those keys must still parse with
// sensible defaults.
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "dve/audio/synthesizer.hpp"

namespace {
using namespace dve::audio;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// A generative preset with every new field set to a non-default value,
// exercising all step sub-fields on a few authored steps per lane.
SynthPreset make_generative_test_preset() {
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Phase3 Serialization Probe";
    auto& seq = preset.sequencer;
    seq.enabled = true;
    seq.channel = 3;
    seq.scale = SequencerScale::Dorian;
    seq.rootNote = 57;
    seq.octaveRange = 3;
    seq.randomSeed = 0xDEADBEEFu;
    const std::uint8_t lengths[7] = {5, 4, 6, 3, 7, 2, 8};
    for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
        auto& lane = seq.lanes[li];
        lane.stepCount = lengths[li];
        lane.direction = static_cast<SequencerDirection>((li + 1U) % 4U);
        lane.mutationAmount = 0.1F * static_cast<float>(li + 1U);
        lane.patternCycles = static_cast<std::uint8_t>(li + 1U);
        // Author two non-default steps per lane with every sub-field touched.
        for (std::size_t si = 0; si < 2; ++si) {
            auto& step = lane.steps[si * 3];
            step.value = (li == 0) ? (-12.0F + 7.0F * static_cast<float>(si))
                                   : (0.10F + 0.10F * static_cast<float>(li + si));  // within [0,1]/[-1,1]
            step.probability = 0.75F - 0.1F * static_cast<float>(si);
            step.ratchets = static_cast<std::uint8_t>(2U + si);
            step.microtiming = -0.25F + 0.5F * static_cast<float>(si);
            step.glide = (si == 0);
            step.accent = 1.25F + 0.25F * static_cast<float>(si);
            step.skip = (si == 1);
            step.condition = static_cast<SequencerStepCondition>((li + si) % 4U);
            step.conditionN = static_cast<std::uint8_t>(3U + li + si);
        }
    }
    preset.genetics.mutationIntensity = 0.7F;
    preset.genetics.mutationSeed = 0x0123456789ABCDEFULL;
    preset.genetics.lockedGroups = 0b10101010;
    preset.attractor.enabled = true;
    preset.attractor.config.bpm = 100.0;
    preset.attractor.config.barsHome = 16.0;
    preset.attractor.config.barsRise = 8.0;
    preset.attractor.config.barsTension = 6.0;
    preset.attractor.config.barsPeak = 4.0;
    preset.attractor.config.barsFall = 10.0;
    preset.attractor.config.seed = 0xFEDCBA9876543210ULL;
    std::string error;
    require(preset.validate(&error), ("probe preset invalid: " + error).c_str());
    return preset;
}

void check_field(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(std::string("round-trip mismatch: ") + name);
}

void test_exact_round_trip() {
    const SynthPreset preset = make_generative_test_preset();
    const std::string text = preset.serialize();
    require(!text.empty(), "serialize produced empty text");
    std::string error;
    auto parsed = SynthPreset::parse(text, &error);
    require(parsed.has_value(), ("parse failed: " + error).c_str());

    // Serialize is deterministic: re-serializing must give byte-identical text.
    check_field(parsed->serialize() == text, "serialize(parse(serialize(p))) != serialize(p)");

    // Spot-check every new field survived exactly.
    const auto& a = preset.sequencer;
    const auto& b = parsed->sequencer;
    check_field(b.enabled && b.channel == 3 && b.scale == SequencerScale::Dorian &&
                b.rootNote == 57 && b.octaveRange == 3 && b.randomSeed == 0xDEADBEEFu,
                "sequencer globals");
    for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
        const auto& la = a.lanes[li];
        const auto& lb = b.lanes[li];
        check_field(lb.stepCount == la.stepCount && lb.direction == la.direction &&
                    lb.mutationAmount == la.mutationAmount && lb.patternCycles == la.patternCycles,
                    "lane header");
        for (std::size_t si = 0; si < 2; ++si) {
            const auto& sa = la.steps[si * 3];
            const auto& sb = lb.steps[si * 3];
            check_field(sb.value == sa.value && sb.probability == sa.probability &&
                        sb.ratchets == sa.ratchets && sb.microtiming == sa.microtiming &&
                        sb.glide == sa.glide && sb.accent == sa.accent && sb.skip == sa.skip &&
                        sb.condition == sa.condition && sb.conditionN == sa.conditionN,
                        "lane step");
        }
    }
    check_field(parsed->genetics.mutationIntensity == 0.7F, "genetics intensity");
    check_field(parsed->genetics.mutationSeed == 0x0123456789ABCDEFULL, "genetics seed");
    check_field(parsed->genetics.lockedGroups == 0b10101010, "genetics locks");
    const auto& ac = parsed->attractor.config;
    check_field(parsed->attractor.enabled, "attractor enabled");
    check_field(ac.bpm == 100.0 && ac.barsHome == 16.0 && ac.barsRise == 8.0 &&
                ac.barsTension == 6.0 && ac.barsPeak == 4.0 && ac.barsFall == 10.0 &&
                ac.seed == 0xFEDCBA9876543210ULL,
                "attractor config");
    std::cout << "exact round-trip: OK (" << text.size() << " bytes)\n";
}

void test_backward_compatibility() {
    // Strip every Phase 3 key: old preset text must still parse, with the
    // sequencer disabled and musical lane defaults in place.
    const std::string text = make_generative_test_preset().serialize();
    std::ostringstream stripped;
    std::istringstream in(text);
    std::string line;
    std::size_t removed = 0;
    while (std::getline(in, line)) {
        if (line.starts_with("seq.") || line.starts_with("gen.") || line.starts_with("attr.")) {
            ++removed;
            continue;
        }
        stripped << line << '\n';
    }
    require(removed > 50, "expected many Phase 3 keys to strip");
    std::string error;
    auto parsed = SynthPreset::parse(stripped.str(), &error);
    require(parsed.has_value(), ("old-style parse failed: " + error).c_str());
    require(parsed->validate(&error), ("old-style preset invalid: " + error).c_str());

    const auto& seq = parsed->sequencer;
    check_field(!seq.enabled, "default sequencer disabled");
    check_field(seq.scale == SequencerScale::Chromatic, "default scale");
    check_field(seq.rootNote == 60 && seq.octaveRange == 2, "default root/octaves");
    check_field(seq.channel == 0, "default channel");
    for (std::size_t li = 0; li < kSequencerLaneCount; ++li) {
        const auto& lane = seq.lanes[li];
        check_field(lane.stepCount == 16, "default step count");
        check_field(lane.direction == SequencerDirection::Forward, "default direction");
        check_field(lane.mutationAmount == 0.0F, "default mutation");
        const SequencerStep expected = default_sequencer_step(static_cast<SequencerLane>(li));
        for (const auto& step : lane.steps) check_field(step == expected, "default step values");
    }
    check_field(parsed->genetics.mutationIntensity == 0.25F, "default genetics intensity");
    check_field(parsed->genetics.lockedGroups == 0, "default genetics locks");
    check_field(!parsed->attractor.enabled, "default attractor disabled");
    check_field(parsed->attractor.config.bpm == 120.0, "default attractor bpm");
    std::cout << "backward compatibility: OK (" << removed << " keys stripped)\n";
}

void test_make_default_round_trip() {
    // A default preset must round-trip byte-identically too (steps equal to
    // the musical lane defaults are omitted and restored).
    const SynthPreset preset = SynthPreset::make_default();
    const std::string text = preset.serialize();
    auto parsed = SynthPreset::parse(text, nullptr);
    require(parsed.has_value(), "default preset parse failed");
    check_field(parsed->serialize() == text, "default preset not byte-stable");
    std::cout << "default round-trip: OK\n";
}

}  // namespace

int main() {
    try {
        test_exact_round_trip();
        test_backward_compatibility();
        test_make_default_round_trip();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "phase3 serialization: all tests passed\n";
    return 0;
}
