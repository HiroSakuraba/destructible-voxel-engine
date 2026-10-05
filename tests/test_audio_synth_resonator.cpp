// Tests for the Phase 2 modal resonator voice generator (SYN-011b).
// Fully synthetic: no audio files. Covers impulse decay vs decaySeconds, mode
// frequency ratios, damping scaling, inharmonicity stretch, brightness tilt,
// the 32-mode cap, excitation sources, name strings, and preset round-trip.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool nearf(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static constexpr float kTestSampleRate = 48000.0F;

// Preset whose only sounding component is osc 0 as a modal resonator, with a
// neutral processing chain so measurements see the raw resonator output.
static SynthPreset resonator_preset() {
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Resonator Test";
    for (std::size_t i = 0; i < preset.oscillators.size(); ++i) {
        preset.oscillators[i].enabled = (i == 0);
        preset.oscillators[i].gain = 1.0F;
    }
    auto& osc = preset.oscillators[0];
    osc.waveform = OscillatorWaveform::ModalResonator;
    osc.modalResonator = ModalResonatorParameters::make_default();
    preset.filter.enabled = false;
    preset.chorus.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = false;
    preset.distortion.enabled = false;
    preset.bitcrusher.enabled = false;
    preset.harmonizer.enabled = false;
    preset.flanger.enabled = false;
    preset.ensemble.enabled = false;
    preset.phaser.enabled = false;
    preset.ampEnvelope.attackSeconds = 0.001F;
    preset.ampEnvelope.decaySeconds = 0.0F;
    preset.ampEnvelope.sustainLevel = 1.0F;
    preset.ampEnvelope.releaseSeconds = 8.0F;
    preset.masterGain = 1.0F;
    return preset;
}

static std::vector<float> render_resonator(SynthPreset preset, std::uint8_t note,
                                           std::uint32_t frames, float velocity = 1.0F) {
    std::string error;
    if (!preset.validate(&error)) {
        std::printf("preset invalid: %s\n", error.c_str());
        ++g_failures;
        return {};
    }
    Synthesizer synth(static_cast<std::uint32_t>(kTestSampleRate));
    synth.set_preset(preset);
    if (!synth.note_on(note, velocity)) {
        std::printf("note_on failed\n");
        ++g_failures;
    }
    std::vector<float> buffer(static_cast<std::size_t>(frames) * 2U);
    synth.render(buffer.data(), frames);
    synth.note_off(note);
    return buffer;
}

static float mono_sample(const std::vector<float>& buffer, std::size_t frame) {
    return (buffer[frame * 2U] + buffer[frame * 2U + 1U]) * 0.5F;
}

static float peak_abs(const std::vector<float>& buffer, std::size_t from, std::size_t to) {
    float peak = 0.0F;
    for (std::size_t f = from; f < to && f * 2U + 1U < buffer.size(); ++f)
        peak = std::max(peak, std::fabs(mono_sample(buffer, f)));
    return peak;
}

static float rms_window(const std::vector<float>& buffer, std::size_t from, std::size_t to) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t f = from; f < to && f * 2U + 1U < buffer.size(); ++f) {
        const float s = mono_sample(buffer, f);
        sum += static_cast<double>(s) * s;
        ++n;
    }
    return n == 0 ? 0.0F : static_cast<float>(std::sqrt(sum / static_cast<double>(n)));
}

static bool buffer_finite(const std::vector<float>& buffer) {
    for (float s : buffer)
        if (!std::isfinite(s)) return false;
    return true;
}

// Time for the envelope to fall 60 dB, measured between two crossings that both
// lie in the linear region: ref is the peak over [refFromSec, refToSec] (chosen
// after the attack and any excitation transient), target is ref/1000. This is
// immune to tanh saturation of the early peak and to the amp-attack ramp, both
// of which corrupt a naive peak-referenced -60 dB time.
static float decay_60db_time(const std::vector<float>& buffer, float refFromSec, float refToSec) {
    const std::size_t refFrom = static_cast<std::size_t>(refFromSec * kTestSampleRate);
    const std::size_t refTo = static_cast<std::size_t>(refToSec * kTestSampleRate);
    const float ref = peak_abs(buffer, refFrom, refTo);
    if (ref <= 0.0F) return -1.0F;
    const float target = ref * 0.001F;
    const std::size_t frames = buffer.size() / 2U;
    std::size_t fRef = refFrom, fTarget = refFrom;
    for (std::size_t f = refFrom; f < frames; ++f) {
        const float a = std::fabs(mono_sample(buffer, f));
        if (a >= ref) fRef = f;
        if (a >= target) fTarget = f;
    }
    if (fTarget <= fRef) return -1.0F;
    return static_cast<float>(fTarget - fRef) / kTestSampleRate;
}

// Upward zero-crossing frequency estimate over [skipSeconds, skipSeconds+windowSeconds].
static float zero_crossing_frequency(const std::vector<float>& buffer, float skipSeconds,
                                     float windowSeconds) {
    const std::size_t start = static_cast<std::size_t>(skipSeconds * kTestSampleRate);
    const std::size_t end = start + static_cast<std::size_t>(windowSeconds * kTestSampleRate);
    std::size_t crossings = 0;
    float prev = mono_sample(buffer, start);
    for (std::size_t f = start + 1; f < end && f * 2U + 1U < buffer.size(); ++f) {
        const float s = mono_sample(buffer, f);
        if (prev <= 0.0F && s > 0.0F) ++crossings;
        prev = s;
    }
    return static_cast<float>(crossings) / windowSeconds;
}

int main() {
    // Test 1: impulse excitation produces decaying output; -60 dB time matches
    // decaySeconds (envelope ~ e^(-t/tau), so t60 = tau * ln(1000)).
    // excitationLevel is kept small so the output tanh stays in its linear
    // region; the decay is measured between two crossings in the linear region.
    {
        SynthPreset preset = resonator_preset();
        auto& mr = preset.oscillators[0].modalResonator;
        mr.modeCount = 1;
        mr.modes[0] = ModalResonatorMode{1.0F, 0.5F, 1.0F};
        mr.excitation = ExcitationSource::Impulse;
        mr.excitationLevel = 0.02F;
        const auto buffer = render_resonator(preset, 69, static_cast<std::uint32_t>(5.0F * kTestSampleRate));
        const float peak = peak_abs(buffer, 0, static_cast<std::size_t>(0.1F * kTestSampleRate));
        std::printf("impulse: peak=%.4f\n", peak);
        CHECK(buffer_finite(buffer));
        CHECK(peak > 0.02F);  // excitation produced sound
        const float t60 = decay_60db_time(buffer, 0.5F, 1.0F);
        const float expected = 0.5F * std::log(1000.0F);
        std::printf("impulse: 60dB decay=%.3fs (expected %.3fs)\n", t60, expected);
        CHECK(t60 > 0.0F && nearf(t60, expected, expected * 0.15F));
    }

    // Test 2: mode frequency ratios are correct (single isolated modes).
    {
        SynthPreset preset = resonator_preset();
        auto& mr = preset.oscillators[0].modalResonator;
        mr.modeCount = 1;
        mr.modes[0] = ModalResonatorMode{2.0F, 3.0F, 1.0F};
        mr.excitation = ExcitationSource::Impulse;
        const auto buffer = render_resonator(preset, 69, static_cast<std::uint32_t>(2.0F * kTestSampleRate));
        const float freq = zero_crossing_frequency(buffer, 0.2F, 1.0F);
        std::printf("ratio 2 @ A4: measured %.2f Hz (expected 880)\n", freq);
        CHECK(nearf(freq, 880.0F, 4.0F));
    }
    {
        // baseFrequency overrides the played note.
        SynthPreset preset = resonator_preset();
        auto& mr = preset.oscillators[0].modalResonator;
        mr.modeCount = 1;
        mr.modes[0] = ModalResonatorMode{1.0F, 3.0F, 1.0F};
        mr.baseFrequency = 330.0F;
        mr.excitation = ExcitationSource::Impulse;
        const auto buffer = render_resonator(preset, 69, static_cast<std::uint32_t>(2.0F * kTestSampleRate));
        const float freq = zero_crossing_frequency(buffer, 0.2F, 1.0F);
        std::printf("baseFrequency 330: measured %.2f Hz\n", freq);
        CHECK(nearf(freq, 330.0F, 4.0F));
    }

    // Test 3: damping scales decay times (damping=0.5 halves the 60 dB time).
    {
        auto make = [](float damping) {
            SynthPreset preset = resonator_preset();
            auto& mr = preset.oscillators[0].modalResonator;
            mr.modeCount = 1;
            mr.modes[0] = ModalResonatorMode{1.0F, 0.5F, 1.0F};
            mr.damping = damping;
            mr.excitation = ExcitationSource::Impulse;
            mr.excitationLevel = 0.02F;
            return preset;
        };
        const auto full = render_resonator(make(1.0F), 69, static_cast<std::uint32_t>(5.0F * kTestSampleRate));
        const auto half = render_resonator(make(0.5F), 69, static_cast<std::uint32_t>(3.0F * kTestSampleRate));
        const float tFull = decay_60db_time(full, 0.5F, 1.0F);
        const float tHalf = decay_60db_time(half, 0.5F, 1.0F);
        std::printf("damping: t(1.0)=%.3fs t(0.5)=%.3fs ratio=%.3f (expected 2.0)\n",
                    tFull, tHalf, tFull / tHalf);
        CHECK(tFull > 0.0F && tHalf > 0.0F);
        CHECK(nearf(tFull / tHalf, 2.0F, 0.2F));
        // Damping 1.0 leaves the requested decay untouched.
        CHECK(nearf(tFull, 0.5F * std::log(1000.0F), 0.5F * std::log(1000.0F) * 0.15F));
    }

    // Test 4: inharmonicity stretches partials upward.
    // ratio' = ratio * sqrt(1 + B*(ratio^2 - 1)): 880 * sqrt(1.3) = 1003.35 at B=0.1.
    {
        auto make = [](float inharmonicity) {
            SynthPreset preset = resonator_preset();
            auto& mr = preset.oscillators[0].modalResonator;
            mr.modeCount = 2;
            mr.modes[0] = ModalResonatorMode{1.0F, 3.0F, 0.0F};  // muted: isolate mode 1
            mr.modes[1] = ModalResonatorMode{2.0F, 3.0F, 1.0F};
            mr.inharmonicity = inharmonicity;
            mr.excitation = ExcitationSource::Impulse;
            return preset;
        };
        const auto plain = render_resonator(make(0.0F), 69, static_cast<std::uint32_t>(2.0F * kTestSampleRate));
        const auto stretched = render_resonator(make(0.1F), 69, static_cast<std::uint32_t>(2.0F * kTestSampleRate));
        const float fPlain = zero_crossing_frequency(plain, 0.2F, 1.0F);
        const float fStretched = zero_crossing_frequency(stretched, 0.2F, 1.0F);
        const float expected = 880.0F * std::sqrt(1.0F + 0.1F * (4.0F - 1.0F));
        std::printf("inharmonicity: plain=%.2f stretched=%.2f (expected %.2f)\n",
                    fPlain, fStretched, expected);
        CHECK(nearf(fPlain, 880.0F, 4.0F));
        CHECK(fStretched > fPlain + 50.0F);
        CHECK(nearf(fStretched, expected, 8.0F));
    }

    // Test 5: brightness tilts high-mode gains.
    // gain_m = gain * (m+1)^(2*(brightness-0.5)): the per-mode gain scales the
    // output twice (excitation coupling x output weight), so comparing the SAME
    // mode at different brightness settings cancels the impulse-response
    // 1/sin(w) factor and the decay. For mode 7 (m+1 = 8) the RMS ratio between
    // brightness 1.0 and 0.5 is (8/1)^2 = 64; between 0.0 and 0.5 it is 1/64.
    // excitationLevel is small to keep the output tanh linear.
    {
        auto make = [](float brightness) {
            SynthPreset preset = resonator_preset();
            auto& mr = preset.oscillators[0].modalResonator;
            mr.modeCount = 8;
            for (std::size_t m = 0; m < 8; ++m)
                mr.modes[m] = ModalResonatorMode{static_cast<float>(m + 1U), 2.0F, 0.0F};
            mr.modes[7] = ModalResonatorMode{8.0F, 2.0F, 1.0F};
            mr.brightness = brightness;
            mr.excitationLevel = 0.01F;
            mr.excitation = ExcitationSource::Impulse;
            return preset;
        };
        auto rms = [](SynthPreset p) {
            const auto buffer = render_resonator(p, 69, static_cast<std::uint32_t>(1.0F * kTestSampleRate));
            CHECK(buffer_finite(buffer));
            return rms_window(buffer, static_cast<std::size_t>(0.1F * kTestSampleRate),
                              static_cast<std::size_t>(0.6F * kTestSampleRate));
        };
        const float rmsFlat = rms(make(0.5F));
        const float rmsBright = rms(make(1.0F));
        const float rmsDark = rms(make(0.0F));
        const float ratioBright = rmsBright / rmsFlat;
        const float ratioDark = rmsDark / rmsFlat;
        std::printf("brightness: mode7 rms b=1.0/0.5=%.1f (expect ~64), b=0.0/0.5=%.4f (expect ~0.0156)\n",
                    ratioBright, ratioDark);
        CHECK(rmsFlat > 1e-4F);
        CHECK(rmsBright > rmsFlat && rmsFlat > rmsDark);  // tilt direction
        CHECK(ratioBright > 30.0F);    // strong brightening of the high mode
        CHECK(ratioDark < 0.05F);      // strong darkening of the high mode
    }

    // Test 6: mode count is capped at 32.
    {
        std::string error;
        SynthPreset preset = resonator_preset();
        preset.oscillators[0].modalResonator.modeCount = 32;
        CHECK(preset.validate(&error));  // 32 is legal
        const auto buffer = render_resonator(preset, 69, static_cast<std::uint32_t>(0.5F * kTestSampleRate));
        CHECK(buffer_finite(buffer));
        CHECK(peak_abs(buffer, 0, static_cast<std::size_t>(0.1F * kTestSampleRate)) > 0.01F);
        preset.oscillators[0].modalResonator.modeCount = 33;
        CHECK(!preset.validate(&error));
        preset.oscillators[0].modalResonator.modeCount = 255;
        CHECK(!preset.validate(&error));
        preset.oscillators[0].modalResonator.modeCount = 0;
        CHECK(!preset.validate(&error));
    }

    // Test 7: name strings.
    {
        CHECK(oscillator_waveform_name(OscillatorWaveform::ModalResonator) == "Modal Resonator");
        CHECK(modal_excitation_source_name(ExcitationSource::Impulse) == "Impulse");
        CHECK(modal_excitation_source_name(ExcitationSource::NoiseBurst) == "Noise Burst");
        CHECK(modal_excitation_source_name(ExcitationSource::Oscillator) == "Oscillator");
        CHECK(modal_excitation_source_name(ExcitationSource::SampleTransient) == "Sample Transient");
    }

    // Test 8: excitation sources.
    {
        // NoiseBurst: sounds, then decays to silence after burst + decay.
        SynthPreset preset = resonator_preset();
        auto& mr = preset.oscillators[0].modalResonator;
        mr.modeCount = 4;
        for (std::size_t m = 0; m < 4; ++m)
            mr.modes[m] = ModalResonatorMode{static_cast<float>(m + 1U), 0.2F, 1.0F};
        mr.excitation = ExcitationSource::NoiseBurst;
        mr.noiseBurstMilliseconds = 40.0F;
        const auto buffer = render_resonator(preset, 69, static_cast<std::uint32_t>(3.0F * kTestSampleRate));
        const float early = rms_window(buffer, static_cast<std::size_t>(0.05F * kTestSampleRate),
                                       static_cast<std::size_t>(0.3F * kTestSampleRate));
        const float late = rms_window(buffer, static_cast<std::size_t>(2.5F * kTestSampleRate),
                                      static_cast<std::size_t>(3.0F * kTestSampleRate));
        std::printf("noiseburst: early=%.4f late=%.6f\n", early, late);
        CHECK(buffer_finite(buffer));
        CHECK(early > 0.01F);
        CHECK(late < 0.002F);
    }
    {
        // Oscillator: dedicated internal exciter sustains a tone at the base
        // frequency while the key is held.
        SynthPreset preset = resonator_preset();
        auto& mr = preset.oscillators[0].modalResonator;
        mr.modeCount = 4;
        for (std::size_t m = 0; m < 4; ++m)
            mr.modes[m] = ModalResonatorMode{static_cast<float>(m + 1U), 2.0F, 1.0F};
        mr.excitation = ExcitationSource::Oscillator;
        const auto buffer = render_resonator(preset, 69, static_cast<std::uint32_t>(1.5F * kTestSampleRate));
        const float sustained = rms_window(buffer, static_cast<std::size_t>(0.5F * kTestSampleRate),
                                           static_cast<std::size_t>(1.0F * kTestSampleRate));
        const float freq = zero_crossing_frequency(buffer, 0.5F, 0.5F);
        std::printf("oscillator excitation: sustained rms=%.4f freq=%.1f Hz\n", sustained, freq);
        CHECK(buffer_finite(buffer));
        CHECK(sustained > 0.01F);
        CHECK(nearf(freq, 440.0F, 25.0F));
    }
    {
        // SampleTransient: attack portion of the resident sample bank (synthetic).
        // The one-shot transient excites the modes, which then decay with their
        // own decaySeconds; excitationLevel is small to keep the tanh linear so
        // the decay rate is measurable.
        SynthPreset preset = resonator_preset();
        auto& mr = preset.oscillators[0].modalResonator;
        mr.modeCount = 4;
        for (std::size_t m = 0; m < 4; ++m)
            mr.modes[m] = ModalResonatorMode{static_cast<float>(m + 1U), 0.3F, 1.0F};
        mr.excitation = ExcitationSource::SampleTransient;
        mr.excitationLevel = 0.02F;
        mr.transientMilliseconds = 60.0F;
        preset.sampleBank.name = "Synthetic Transient";
        preset.sampleBank.enabled = true;
        preset.sampleBank.sampleRate = 48000;
        preset.sampleBank.frameCount = 4800;
        std::uint32_t lcg = 0x12345678U;
        for (std::uint32_t f = 0; f < preset.sampleBank.frameCount; ++f) {
            lcg = lcg * 1664525U + 1013904223U;
            const float n = static_cast<float>(lcg >> 8) / 8388608.0F - 1.0F;
            const float env = std::exp(-static_cast<float>(f) / (0.01F * 48000.0F));
            preset.sampleBank.samples[f] = n * env * 0.8F;
        }
        const auto buffer = render_resonator(preset, 69, static_cast<std::uint32_t>(4.0F * kTestSampleRate));
        const float early = rms_window(buffer, static_cast<std::size_t>(0.01F * kTestSampleRate),
                                       static_cast<std::size_t>(0.06F * kTestSampleRate));
        const float t60 = decay_60db_time(buffer, 0.5F, 1.0F);
        const float expected = 0.3F * std::log(1000.0F);
        std::printf("sampletransient: early=%.4f 60dB decay=%.3fs (expected %.3fs)\n", early, t60, expected);
        CHECK(buffer_finite(buffer));
        CHECK(early > 0.005F);  // the transient excited the bank
        CHECK(t60 > 0.0F && nearf(t60, expected, expected * 0.20F));
    }
    {
        // SampleTransient with no sample loaded falls back to a noise burst.
        SynthPreset preset = resonator_preset();
        auto& mr = preset.oscillators[0].modalResonator;
        mr.modeCount = 4;
        for (std::size_t m = 0; m < 4; ++m)
            mr.modes[m] = ModalResonatorMode{static_cast<float>(m + 1U), 0.3F, 1.0F};
        mr.excitation = ExcitationSource::SampleTransient;
        const auto buffer = render_resonator(preset, 69, static_cast<std::uint32_t>(1.0F * kTestSampleRate));
        const float early = rms_window(buffer, static_cast<std::size_t>(0.05F * kTestSampleRate),
                                       static_cast<std::size_t>(0.3F * kTestSampleRate));
        std::printf("sampletransient fallback: early=%.4f\n", early);
        CHECK(buffer_finite(buffer));
        CHECK(early > 0.01F);
    }

    // Test 9: preset text serialization round-trip.
    {
        SynthPreset preset = resonator_preset();
        preset.name = "Resonator Round Trip";
        auto& mr = preset.oscillators[0].modalResonator;
        mr.excitation = ExcitationSource::NoiseBurst;
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
        std::string error;
        CHECK(preset.validate(&error));
        const std::string text = preset.serialize();
        CHECK(text.find("osc0.wave=modalresonator") != std::string::npos);
        CHECK(text.find("osc0.modalExcitation=noiseburst") != std::string::npos);
        CHECK(text.find("osc0.modalModeCount=5") != std::string::npos);
        CHECK(text.find("osc0.modalMode0=1,2,1") != std::string::npos);
        CHECK(text.find("osc0.modalMode31=7.77,0.11,0.22") != std::string::npos);
        const auto decoded = SynthPreset::parse(text, &error);
        if (!decoded) {
            std::printf("parse failed: %s\n", error.c_str());
            ++g_failures;
        } else {
            CHECK(decoded->oscillators[0].waveform == OscillatorWaveform::ModalResonator);
            const auto& d = decoded->oscillators[0].modalResonator;
            CHECK(d.excitation == ExcitationSource::NoiseBurst);
            CHECK(d.modeCount == 5);
            CHECK(nearf(d.baseFrequency, 220.5F, 1e-3F));
            CHECK(nearf(d.damping, 0.7F, 1e-4F));
            CHECK(nearf(d.inharmonicity, 0.02F, 1e-4F));
            CHECK(nearf(d.brightness, 0.8F, 1e-4F));
            CHECK(nearf(d.excitationLevel, 0.9F, 1e-4F));
            CHECK(nearf(d.noiseBurstMilliseconds, 25.0F, 1e-3F));
            CHECK(nearf(d.transientMilliseconds, 120.0F, 1e-3F));
            CHECK(nearf(d.modes[0].frequencyRatio, 1.0F, 1e-4F));
            CHECK(nearf(d.modes[1].frequencyRatio, 2.01F, 1e-4F));
            CHECK(nearf(d.modes[1].decaySeconds, 1.5F, 1e-4F));
            CHECK(nearf(d.modes[1].gain, 0.7F, 1e-4F));
            CHECK(nearf(d.modes[2].frequencyRatio, 2.98F, 1e-4F));
            CHECK(nearf(d.modes[4].gain, 0.2F, 1e-4F));
            CHECK(nearf(d.modes[31].frequencyRatio, 7.77F, 1e-3F));
            CHECK(nearf(d.modes[31].decaySeconds, 0.11F, 1e-4F));
            CHECK(nearf(d.modes[31].gain, 0.22F, 1e-4F));
            CHECK(decoded->validate(&error));
        }
    }
    {
        // Legacy version-5 presets without modal fields keep modal defaults.
        std::string error;
        const auto legacy = SynthPreset::parse(
            "DVE_SYNTH_PRESET=5\nname=Legacy Resonator\nosc0.wave=modalresonator\n", &error);
        CHECK(legacy.has_value());
        if (legacy) {
            CHECK(legacy->oscillators[0].waveform == OscillatorWaveform::ModalResonator);
            const auto& d = legacy->oscillators[0].modalResonator;
            CHECK(d.modeCount == 12);
            CHECK(d.excitation == ExcitationSource::Impulse);
            CHECK(nearf(d.modes[0].frequencyRatio, 1.0F, 1e-6F));
            CHECK(nearf(d.modes[11].frequencyRatio, 12.0F, 1e-6F));
        }
    }

    if (g_failures == 0) std::printf("resonator tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
