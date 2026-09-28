// Comprehensive Phase 2 text-serialization audit.
// One preset exercising EVERY new Phase 2 parameter at once must round-trip
// through SynthPreset::serialize()/parse() with every field intact, and the
// parsed result must validate(). A second pass covers the remaining enum
// values (SoftClip, Formant topology, OneShot/Forward, NoiseBurst excitation).
#include <cmath>
#include <cstdio>
#include <string>

#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool nearly(float a, float b, float tol = 1e-4F) {
    return std::fabs(a - b) <= tol * (1.0F + std::fabs(b));
}

// A preset touching every new Phase 2 knob: sampler generator, modal
// resonator generator, comb/formant filter parameters + oversampling,
// tempo-synced delay, diffusion delay, and the new distortion modes.
static SynthPreset phase2_kitchen_sink() {
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Phase2 Kitchen Sink";

    auto& samplerOsc = preset.oscillators[0];
    samplerOsc.waveform = OscillatorWaveform::Sampler;
    samplerOsc.gain = 0.9F;
    preset.sampler.enabled = true;
    preset.sampler.sampleIndex = 0;
    preset.sampler.playbackMode = SamplerPlaybackMode::Loop;
    preset.sampler.direction = SamplerDirection::Reverse;
    preset.sampler.loopStartSeconds = 0.05F;
    preset.sampler.loopEndSeconds = 0.20F;
    preset.sampler.loopCrossfadeSeconds = 0.01F;
    preset.sampler.pitchTracking = false;
    preset.sampler.startOffsetSeconds = 0.01F;
    preset.sampler.gain = 0.7F;

    auto& resonatorOsc = preset.oscillators[1];
    resonatorOsc.waveform = OscillatorWaveform::ModalResonator;
    auto& mr = resonatorOsc.modalResonator;
    mr.excitation = ExcitationSource::Oscillator;
    mr.modeCount = 5;
    mr.baseFrequency = 220.5F;
    mr.damping = 0.7F;
    mr.inharmonicity = 0.02F;
    mr.brightness = 0.8F;
    mr.excitationLevel = 0.9F;
    mr.noiseBurstMilliseconds = 25.0F;
    mr.transientMilliseconds = 120.0F;
    mr.modes[0] = ModalResonatorMode{1.0F, 2.0F, 1.0F};
    mr.modes[1] = ModalResonatorMode{2.01F, 1.5F, 0.7F};
    mr.modes[2] = ModalResonatorMode{2.98F, 1.0F, 0.5F};
    mr.modes[3] = ModalResonatorMode{4.2F, 0.8F, 0.3F};
    mr.modes[4] = ModalResonatorMode{5.4F, 0.6F, 0.2F};
    mr.modes[31] = ModalResonatorMode{7.77F, 0.11F, 0.22F};

    preset.filter.topology = FilterTopology::Comb;
    preset.filter.comb.damping = 0.4F;
    preset.filter.comb.mix = 0.8F;
    preset.filter.comb.feedbackScale = 1.2F;
    preset.filter.formant.frequencyHertz = {800.0F, 1150.0F, 2500.0F, 3600.0F};
    preset.filter.formant.gains = {0.9F, 0.7F, 0.4F, 0.25F};
    preset.filter.formant.dryMix = 0.3F;
    preset.filter.oversampling = FilterOversampling::X4;

    preset.delay.tempoSync = true;
    preset.delay.syncBeats = 0.75F;

    preset.diffusionDelay.enabled = true;
    preset.diffusionDelay.timeSeconds = 0.42F;
    preset.diffusionDelay.feedback = 0.5F;
    preset.diffusionDelay.mix = 0.3F;
    preset.diffusionDelay.diffusion = 0.8F;

    preset.distortion.enabled = true;
    preset.distortion.mode = DistortionMode::Foldback;
    preset.distortion.drive = 2.5F;
    preset.distortion.mix = 0.4F;

    auto& slot = preset.modulation[0];
    slot.enabled = true;
    slot.source = ModulationSource::Macro1;
    slot.destination = ModulationDestination::SamplerStartPosition;
    slot.amount = 0.5F;
    slot.bias = -0.25F;  // Phase 0 bias field
    slot.curve = ModulationCurve::Cubic;
    slot.polarity = ModulationPolarity::Unipolar;
    slot.smoothingMilliseconds = 12.0F;

    preset.macros.values[0] = 0.75F;
    preset.macros.names[1] = "Cutoff";
    return preset;
}

static void check_kitchen_sink(const SynthPreset& decoded) {
    const SamplerParameters& s = decoded.sampler;
    CHECK(s.enabled);
    CHECK(s.sampleIndex == 0);
    CHECK(s.playbackMode == SamplerPlaybackMode::Loop);
    CHECK(s.direction == SamplerDirection::Reverse);
    CHECK(nearly(s.loopStartSeconds, 0.05F));
    CHECK(nearly(s.loopEndSeconds, 0.20F));
    CHECK(nearly(s.loopCrossfadeSeconds, 0.01F));
    CHECK(!s.pitchTracking);
    CHECK(nearly(s.startOffsetSeconds, 0.01F));
    CHECK(nearly(s.gain, 0.7F));
    CHECK(decoded.oscillators[0].waveform == OscillatorWaveform::Sampler);
    CHECK(nearly(decoded.oscillators[0].gain, 0.9F));

    CHECK(decoded.oscillators[1].waveform == OscillatorWaveform::ModalResonator);
    const ModalResonatorParameters& d = decoded.oscillators[1].modalResonator;
    CHECK(d.excitation == ExcitationSource::Oscillator);
    CHECK(d.modeCount == 5);
    CHECK(nearly(d.baseFrequency, 220.5F));
    CHECK(nearly(d.damping, 0.7F));
    CHECK(nearly(d.inharmonicity, 0.02F));
    CHECK(nearly(d.brightness, 0.8F));
    CHECK(nearly(d.excitationLevel, 0.9F));
    CHECK(nearly(d.noiseBurstMilliseconds, 25.0F));
    CHECK(nearly(d.transientMilliseconds, 120.0F));
    CHECK(nearly(d.modes[0].frequencyRatio, 1.0F));
    CHECK(nearly(d.modes[1].frequencyRatio, 2.01F));
    CHECK(nearly(d.modes[1].decaySeconds, 1.5F));
    CHECK(nearly(d.modes[1].gain, 0.7F));
    CHECK(nearly(d.modes[4].gain, 0.2F));
    CHECK(nearly(d.modes[31].frequencyRatio, 7.77F));
    CHECK(nearly(d.modes[31].decaySeconds, 0.11F));
    CHECK(nearly(d.modes[31].gain, 0.22F));

    CHECK(decoded.filter.topology == FilterTopology::Comb);
    CHECK(nearly(decoded.filter.comb.damping, 0.4F));
    CHECK(nearly(decoded.filter.comb.mix, 0.8F));
    CHECK(nearly(decoded.filter.comb.feedbackScale, 1.2F));
    CHECK(nearly(decoded.filter.formant.frequencyHertz[0], 800.0F));
    CHECK(nearly(decoded.filter.formant.frequencyHertz[3], 3600.0F));
    CHECK(nearly(decoded.filter.formant.gains[1], 0.7F));
    CHECK(nearly(decoded.filter.formant.dryMix, 0.3F));
    CHECK(decoded.filter.oversampling == FilterOversampling::X4);

    CHECK(decoded.delay.tempoSync);
    CHECK(nearly(decoded.delay.syncBeats, 0.75F));

    CHECK(decoded.diffusionDelay.enabled);
    CHECK(nearly(decoded.diffusionDelay.timeSeconds, 0.42F));
    CHECK(nearly(decoded.diffusionDelay.feedback, 0.5F));
    CHECK(nearly(decoded.diffusionDelay.mix, 0.3F));
    CHECK(nearly(decoded.diffusionDelay.diffusion, 0.8F));

    CHECK(decoded.distortion.enabled);
    CHECK(decoded.distortion.mode == DistortionMode::Foldback);
    CHECK(nearly(decoded.distortion.drive, 2.5F));
    CHECK(nearly(decoded.distortion.mix, 0.4F));

    const ModulationSlot& slot = decoded.modulation[0];
    CHECK(slot.enabled);
    CHECK(slot.source == ModulationSource::Macro1);
    CHECK(slot.destination == ModulationDestination::SamplerStartPosition);
    CHECK(nearly(slot.amount, 0.5F));
    CHECK(nearly(slot.bias, -0.25F));
    CHECK(slot.curve == ModulationCurve::Cubic);
    CHECK(slot.polarity == ModulationPolarity::Unipolar);
    CHECK(nearly(slot.smoothingMilliseconds, 12.0F));

    CHECK(nearly(decoded.macros.values[0], 0.75F));
    CHECK(decoded.macros.names[1] == "Cutoff");
}

int main() {
    setbuf(stdout, nullptr);
    // Pass 1: everything at once.
    {
        const SynthPreset preset = phase2_kitchen_sink();
        std::string error;
        CHECK(preset.validate(&error));
        if (!error.empty()) std::printf("validate error: %s\n", error.c_str());
        const std::string text = preset.serialize();
        CHECK(text.find("osc0.wave=sampler") != std::string::npos);
        CHECK(text.find("osc1.wave=modalresonator") != std::string::npos);
        CHECK(text.find("filter.topology=comb") != std::string::npos);
        CHECK(text.find("distortion.mode=3") != std::string::npos);
        CHECK(text.find("delay.tempoSync=1") != std::string::npos);
        CHECK(text.find("diffusionDelay.diffusion=0.8") != std::string::npos);
        CHECK(text.find("mod0.bias=-0.25") != std::string::npos);
        auto decoded = SynthPreset::parse(text, &error);
        CHECK(decoded.has_value());
        if (!decoded) { std::printf("parse error: %s\n", error.c_str()); return 1; }
        check_kitchen_sink(*decoded);
        CHECK(decoded->name == "Phase2 Kitchen Sink");
        CHECK(decoded->validate(&error));
        if (!error.empty()) std::printf("decoded validate error: %s\n", error.c_str());
    }

    // Pass 2: the remaining Phase 2 enum values round-trip too.
    {
        SynthPreset preset = phase2_kitchen_sink();
        preset.distortion.mode = DistortionMode::SoftClip;
        preset.filter.topology = FilterTopology::Formant;
        preset.filter.oversampling = FilterOversampling::X2;
        preset.sampler.playbackMode = SamplerPlaybackMode::OneShot;
        preset.sampler.direction = SamplerDirection::Forward;
        preset.sampler.pitchTracking = true;
        preset.oscillators[1].modalResonator.excitation = ExcitationSource::NoiseBurst;
        preset.oscillators[1].modalResonator.baseFrequency = 0.0F;  // follow the played note
        preset.delay.tempoSync = false;
        preset.delay.syncBeats = 1.5F;
        preset.diffusionDelay.diffusion = 0.0F;
        std::string error;
        const auto decoded = SynthPreset::parse(preset.serialize(), &error);
        CHECK(decoded.has_value());
        if (!decoded) { std::printf("parse error (pass 2): %s\n", error.c_str()); return 1; }
        CHECK(decoded->distortion.mode == DistortionMode::SoftClip);
        CHECK(decoded->filter.topology == FilterTopology::Formant);
        CHECK(decoded->filter.oversampling == FilterOversampling::X2);
        CHECK(decoded->sampler.playbackMode == SamplerPlaybackMode::OneShot);
        CHECK(decoded->sampler.direction == SamplerDirection::Forward);
        CHECK(decoded->sampler.pitchTracking);
        CHECK(decoded->oscillators[1].modalResonator.excitation == ExcitationSource::NoiseBurst);
        CHECK(nearly(decoded->oscillators[1].modalResonator.baseFrequency, 0.0F));
        CHECK(!decoded->delay.tempoSync);
        CHECK(nearly(decoded->delay.syncBeats, 1.5F));
        CHECK(nearly(decoded->diffusionDelay.diffusion, 0.0F));
        CHECK(decoded->validate(&error));
    }

    // Hand-written texts: every distortion mode number parses, mode 4 is rejected.
    {
        std::string error;
        for (unsigned mode = 0; mode <= 3U; ++mode) {
            const auto parsed = SynthPreset::parse(
                "DVE_SYNTH_PRESET=5\nname=Mode " + std::to_string(mode) +
                "\ndistortion.enabled=1\ndistortion.mode=" + std::to_string(mode) + "\n", &error);
            CHECK(parsed.has_value());
            if (parsed) CHECK(static_cast<unsigned>(parsed->distortion.mode) == mode);
        }
        const auto bad = SynthPreset::parse(
            "DVE_SYNTH_PRESET=5\nname=Bad\ndistortion.mode=4\n", &error);
        CHECK(!bad.has_value());
    }

    // Oversampling values 1/2/4 parse; 3 and 8 are rejected.
    {
        std::string error;
        for (const char* value : {"1", "2", "4"}) {
            const auto parsed = SynthPreset::parse(
                std::string("DVE_SYNTH_PRESET=5\nname=OS\nfilter.oversampling=") + value + "\n", &error);
            CHECK(parsed.has_value());
        }
        for (const char* value : {"0", "3", "8"}) {
            const auto parsed = SynthPreset::parse(
                std::string("DVE_SYNTH_PRESET=5\nname=OS\nfilter.oversampling=") + value + "\n", &error);
            CHECK(!parsed.has_value());
        }
    }

    if (g_failures == 0) std::printf("phase2 serialization audit: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
