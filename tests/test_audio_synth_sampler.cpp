// Tests for the Phase 2 sampler voice generator (SYN-011a).
// Synthetic sample data is generated in-memory; no audio files on disk.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr std::uint32_t kRate = 48000U;
constexpr std::uint32_t kFrames = 4800U;  // 0.1 s at 48 kHz

SynthPreset sampler_preset() {
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Sampler Test";
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    auto& oscillator = preset.oscillators[0];
    oscillator.enabled = true;
    oscillator.waveform = OscillatorWaveform::Sampler;
    oscillator.gain = 1.0F;
    oscillator.semitones = 0.0F;  // neutralize make_default's detuned stack
    oscillator.cents = 0.0F;
    preset.ampEnvelope.attackSeconds = 0.001F;
    preset.ampEnvelope.decaySeconds = 0.001F;
    preset.ampEnvelope.sustainLevel = 1.0F;
    preset.ampEnvelope.releaseSeconds = 0.05F;
    preset.tuning.analogDriftCents = 0.0F;
    preset.filter.enabled = false;
    preset.distortion.enabled = false;
    preset.eq.enabled = false;
    preset.chorus.enabled = false;
    preset.phaser.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = false;
    preset.masterGain = 0.8F;
    preset.sampler.enabled = true;
    preset.sampler.gain = 0.8F;
    return preset;
}

void install_sine_bank(SynthPreset& preset, float frequencyHertz) {
    preset.sampleBank.enabled = true;
    preset.sampleBank.sampleRate = kRate;
    preset.sampleBank.rootNote = 60;
    preset.sampleBank.frameCount = kFrames;
    for (std::uint32_t i = 0; i < kFrames; ++i)
        preset.sampleBank.samples[i] =
            0.9F * std::sin(2.0F * 3.14159265358979F * frequencyHertz *
                            static_cast<float>(i) / static_cast<float>(kRate));
}

void install_ramp_bank(SynthPreset& preset) {
    preset.sampleBank.enabled = true;
    preset.sampleBank.sampleRate = kRate;
    preset.sampleBank.rootNote = 60;
    preset.sampleBank.frameCount = kFrames;
    for (std::uint32_t i = 0; i < kFrames; ++i)
        preset.sampleBank.samples[i] = static_cast<float>(i) / static_cast<float>(kFrames - 1U);
}

double rms_window(const std::vector<float>& interleaved, std::size_t firstFrame, std::size_t frameCount) {
    double sum = 0.0;
    for (std::size_t n = 0; n < frameCount; ++n) {
        const float s = interleaved[(firstFrame + n) * 2U];
        sum += static_cast<double>(s) * static_cast<double>(s);
    }
    return std::sqrt(sum / static_cast<double>(frameCount));
}

double mean_window(const std::vector<float>& interleaved, std::size_t firstFrame, std::size_t frameCount) {
    double sum = 0.0;
    for (std::size_t n = 0; n < frameCount; ++n) sum += interleaved[(firstFrame + n) * 2U];
    return sum / static_cast<double>(frameCount);
}

bool any_voice_active(const Synthesizer& synth) {
    for (const auto& voice : synth.voices())
        if (voice.active) return true;
    return false;
}

// Zero-crossing frequency estimate over a frame window (left channel).
double crossing_frequency(const std::vector<float>& interleaved, std::size_t firstFrame,
                          std::size_t frameCount) {
    std::size_t crossings = 0;
    for (std::size_t n = 1; n < frameCount; ++n) {
        const float a = interleaved[(firstFrame + n - 1U) * 2U];
        const float b = interleaved[(firstFrame + n) * 2U];
        if ((a < 0.0F) != (b < 0.0F)) ++crossings;
    }
    const double seconds = static_cast<double>(frameCount) / static_cast<double>(kRate);
    return static_cast<double>(crossings) / (2.0 * seconds);
}

}  // namespace

int main() {
    setbuf(stdout, nullptr);  // unbuffered: pin down hangs precisely
    // Test 1: one-shot plays, then releases the voice at the sample end.
    {
        SynthPreset preset = sampler_preset();
        install_sine_bank(preset, 440.0F);
        preset.sampler.playbackMode = SamplerPlaybackMode::OneShot;
        preset.sampler.direction = SamplerDirection::Forward;
        Synthesizer synth(kRate);
        synth.set_preset(preset);
        synth.note_on(60, 1.0F);
        std::vector<float> buffer(12000U * 2U);
        synth.render(buffer.data(), 12000U);
        const double early = rms_window(buffer, 100U, 900U);
        const double late = rms_window(buffer, 11000U, 1000U);
        std::printf("oneshot early rms=%.3f late rms=%.4f\n", early, late);
        CHECK(early > 0.1);
        CHECK(late < 0.01);
        CHECK(!any_voice_active(synth));
    }

    // Test 2: loop mode wraps and keeps sounding.
    {
        SynthPreset preset = sampler_preset();
        install_sine_bank(preset, 440.0F);
        preset.sampler.playbackMode = SamplerPlaybackMode::Loop;
        preset.sampler.loopStartSeconds = 0.0F;
        preset.sampler.loopEndSeconds = 0.1F;
        preset.sampler.loopCrossfadeSeconds = 0.0F;
        preset.ampEnvelope.decaySeconds = 0.0F;  // flat sustain: isolate loop periodicity
        preset.ampEnvelope.sustainLevel = 1.0F;
        Synthesizer synth(kRate);
        synth.set_preset(preset);
        synth.note_on(60, 1.0F);
        std::vector<float> buffer(14400U * 2U);
        synth.render(buffer.data(), 14400U);
        const double third = rms_window(buffer, 9600U, 4800U);
        std::printf("loop third-chunk rms=%.3f\n", third);
        CHECK(third > 0.1);
        CHECK(any_voice_active(synth));  // key still held: voice sustains
        double diff = 0.0;
        for (std::size_t n = 0; n < 4800U; ++n)
            diff += std::abs(buffer[(1000U + n) * 2U] - buffer[(5800U + n) * 2U]);
        diff /= 4800.0;
        std::printf("loop period mean abs diff=%.2e\n", diff);
        CHECK(diff < 1e-3);
    }

    // Test 3: reverse plays the sample backwards (ramp: output falls over time).
    {
        SynthPreset preset = sampler_preset();
        install_ramp_bank(preset);
        preset.sampler.playbackMode = SamplerPlaybackMode::OneShot;
        preset.sampler.direction = SamplerDirection::Forward;
        preset.sampler.pitchTracking = false;
        Synthesizer forward(kRate);
        forward.set_preset(preset);
        forward.note_on(60, 1.0F);
        std::vector<float> fwd(3000U * 2U);
        forward.render(fwd.data(), 3000U);

        preset.sampler.direction = SamplerDirection::Reverse;
        Synthesizer backward(kRate);
        backward.set_preset(preset);
        backward.note_on(60, 1.0F);
        std::vector<float> rev(3000U * 2U);
        backward.render(rev.data(), 3000U);

        const double fwdMean = mean_window(fwd, 500U, 1000U);
        const double revMean = mean_window(rev, 500U, 1000U);
        std::printf("ramp fwd mean=%.3f rev mean=%.3f\n", fwdMean, revMean);
        CHECK(fwdMean > 0.05 && fwdMean < 0.45);
        CHECK(revMean > fwdMean * 2.0);
    }

    // Test 4: pitch tracking changes playback rate (zero-crossing measurement).
    {
        for (const bool tracking : {true, false}) {
            SynthPreset preset = sampler_preset();
            install_sine_bank(preset, 440.0F);
            preset.sampler.playbackMode = SamplerPlaybackMode::Loop;
            preset.sampler.loopStartSeconds = 0.0F;
            preset.sampler.loopEndSeconds = 0.1F;
            preset.sampler.pitchTracking = tracking;
            for (const std::uint8_t note : {60, 72}) {
                Synthesizer synth(kRate);
                synth.set_preset(preset);
                synth.note_on(note, 1.0F);
                std::vector<float> buffer(24000U * 2U);
                synth.render(buffer.data(), 24000U);
                const double freq = crossing_frequency(buffer, 4800U, 14400U);
                std::printf("tracking=%d note=%u measured=%.1f Hz\n", tracking ? 1 : 0, note, freq);
                if (tracking) {
                    const double expected = (note == 60) ? 440.0 : 880.0;
                    CHECK(std::abs(freq - expected) < 15.0);
                } else {
                    CHECK(std::abs(freq - 440.0) < 15.0);
                }
                synth.note_off(note);
            }
        }
    }

    // Test 5: crossfaded loop has no discontinuity at the wrap point.
    {
        auto max_wrap_jump = [](float crossfadeSeconds) {
            SynthPreset preset = sampler_preset();
            // 442.6 Hz over 0.1 s: sample ends near +peak while it starts at 0.
            install_sine_bank(preset, 442.6F);
            preset.sampler.playbackMode = SamplerPlaybackMode::Loop;
            preset.sampler.loopStartSeconds = 0.0F;
            preset.sampler.loopEndSeconds = 0.1F;
            preset.sampler.loopCrossfadeSeconds = crossfadeSeconds;
            Synthesizer synth(kRate);
            synth.set_preset(preset);
            synth.note_on(60, 1.0F);
            std::vector<float> buffer(12000U * 2U);
            synth.render(buffer.data(), 12000U);
            double worst = 0.0;
            for (const std::size_t wrap : {4800U, 9600U}) {
                for (std::size_t n = wrap - 96U; n < wrap + 96U; ++n) {
                    const double jump = std::abs(static_cast<double>(buffer[n * 2U]) -
                                                 static_cast<double>(buffer[(n - 1U) * 2U]));
                    worst = std::max(worst, jump);
                }
            }
            return worst;
        };
        const double noXf = max_wrap_jump(0.0F);
        const double xf = max_wrap_jump(0.01F);
        std::printf("wrap jump: no-xfade=%.3f xfade=%.4f\n", noXf, xf);
        CHECK(noXf > 0.2);          // the test sample really is discontinuous without help
        CHECK(xf < noXf * 0.75);    // crossfade substantially reduces the wrap click
        CHECK(xf < 0.2);
    }

    // Test 6: serialization round-trip of every sampler parameter.
    {
        SynthPreset preset = sampler_preset();
        install_sine_bank(preset, 440.0F);
        preset.sampler.enabled = true;
        preset.sampler.sampleIndex = 0;
        preset.sampler.playbackMode = SamplerPlaybackMode::Loop;
        preset.sampler.direction = SamplerDirection::Reverse;
        preset.sampler.loopStartSeconds = 0.012F;
        preset.sampler.loopEndSeconds = 0.083F;
        preset.sampler.loopCrossfadeSeconds = 0.007F;
        preset.sampler.pitchTracking = false;
        preset.sampler.startOffsetSeconds = 0.02F;
        preset.sampler.gain = 0.63F;
        preset.oscillators[3].waveform = OscillatorWaveform::Sampler;
        const std::string text = preset.serialize();
        std::string error;
        auto parsed = SynthPreset::parse(text, &error);
        CHECK(parsed.has_value());
        if (!parsed) std::printf("parse error: %s\n", error.c_str());
        if (parsed) {
            const SamplerParameters& s = parsed->sampler;
            CHECK(s.enabled == true);
            CHECK(s.sampleIndex == 0);
            CHECK(s.playbackMode == SamplerPlaybackMode::Loop);
            CHECK(s.direction == SamplerDirection::Reverse);
            CHECK(std::abs(s.loopStartSeconds - 0.012F) < 1e-6F);
            CHECK(std::abs(s.loopEndSeconds - 0.083F) < 1e-6F);
            CHECK(std::abs(s.loopCrossfadeSeconds - 0.007F) < 1e-6F);
            CHECK(s.pitchTracking == false);
            CHECK(std::abs(s.startOffsetSeconds - 0.02F) < 1e-6F);
            CHECK(std::abs(s.gain - 0.63F) < 1e-6F);
            CHECK(parsed->oscillators[3].waveform == OscillatorWaveform::Sampler);
            CHECK(parsed->validate(&error));
            if (!error.empty()) std::printf("validate error: %s\n", error.c_str());
        }
        // Name strings and mod-destination token round-trip.
        CHECK(modulation_destination_name(ModulationDestination::SamplerStartPosition) == "sampler_start_pos");
        CHECK(sampler_playback_mode_name(SamplerPlaybackMode::Loop) == "Loop");
        CHECK(sampler_direction_name(SamplerDirection::Reverse) == "Reverse");
    }

    // Test 7: edge cases — empty sample, loop points past the end, huge start offset.
    {
        // Empty / disabled bank: silent, no crash.
        {
            SynthPreset preset = sampler_preset();
            preset.sampleBank.enabled = false;
            Synthesizer synth(kRate);
            synth.set_preset(preset);
            synth.note_on(60, 1.0F);
            std::vector<float> buffer(1024U * 2U, 1.0F);
            synth.render(buffer.data(), 1024U);
            CHECK(rms_window(buffer, 0U, 1024U) == 0.0);
        }
        // Loop points beyond the sample length: clamped, keeps playing.
        {
            SynthPreset preset = sampler_preset();
            install_sine_bank(preset, 440.0F);
            preset.sampler.playbackMode = SamplerPlaybackMode::Loop;
            preset.sampler.loopStartSeconds = 5.0F;
            preset.sampler.loopEndSeconds = 10.0F;
            Synthesizer synth(kRate);
            synth.set_preset(preset);
            synth.note_on(60, 1.0F);
            std::vector<float> buffer(9600U * 2U);
            synth.render(buffer.data(), 9600U);
            CHECK(rms_window(buffer, 4800U, 4800U) > 0.1);
            CHECK(any_voice_active(synth));
        }
        // Start offset beyond the sample: clamped to the end, voice releases promptly.
        {
            SynthPreset preset = sampler_preset();
            install_sine_bank(preset, 440.0F);
            preset.sampler.playbackMode = SamplerPlaybackMode::OneShot;
            preset.sampler.startOffsetSeconds = 5.0F;
            Synthesizer synth(kRate);
            synth.set_preset(preset);
            synth.note_on(60, 1.0F);
            std::vector<float> buffer(12000U * 2U);
            synth.render(buffer.data(), 12000U);
            CHECK(!any_voice_active(synth));
        }
    }

    // Test 8: SamplerStartPosition modulation destination moves the start offset.
    {
        SynthPreset preset = sampler_preset();
        install_ramp_bank(preset);
        preset.sampler.playbackMode = SamplerPlaybackMode::OneShot;
        preset.sampler.direction = SamplerDirection::Forward;
        preset.sampler.pitchTracking = false;
        ModulationSlot slot{};
        slot.enabled = true;
        slot.source = ModulationSource::Velocity;
        slot.destination = ModulationDestination::SamplerStartPosition;
        slot.amount = 0.5F;
        slot.bias = 0.0F;  // Phase-0 field
        slot.curve = ModulationCurve::Linear;
        slot.polarity = ModulationPolarity::Unipolar;
        slot.smoothingMilliseconds = 0.0F;
        preset.modulation[0] = slot;

        Synthesizer modded(kRate);
        modded.set_preset(preset);
        modded.note_on(60, 1.0F);  // velocity 1 -> start = 0.5 * duration
        std::vector<float> withMod(2000U * 2U);
        modded.render(withMod.data(), 2000U);

        preset.modulation[0].enabled = false;
        Synthesizer plain(kRate);
        plain.set_preset(preset);
        plain.note_on(60, 1.0F);
        std::vector<float> noMod(2000U * 2U);
        plain.render(noMod.data(), 2000U);

        const double modMean = mean_window(withMod, 500U, 1000U);
        const double plainMean = mean_window(noMod, 500U, 1000U);
        std::printf("start-pos mod: with=%.3f without=%.3f\n", modMean, plainMean);
        CHECK(modMean > plainMean * 1.8);
    }

    if (g_failures == 0) std::printf("ALL SAMPLER TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
