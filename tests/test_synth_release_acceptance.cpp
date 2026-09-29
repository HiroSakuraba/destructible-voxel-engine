// Section 36: definition of the first serious release.
//
// One deterministic acceptance executable. Seven checks, each against the real
// DSP — no mocks. If any check fails for a real DSP reason, that failure is
// the deliverable: it tells us what to fix before calling this a release.
//
// House style: plain int main(), require() throwing std::runtime_error,
// PASS/FAIL lines on stdout.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/audio_features.hpp"
#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Single saw oscillator, flat filter, no modulation: the pitch reference.
SynthPreset flat_single_saw() {
    SynthPreset preset = SynthPreset::make_default();
    for (std::size_t i = 1; i < preset.oscillators.size(); ++i) preset.oscillators[i].enabled = false;
    auto& osc = preset.oscillators[0];
    osc.waveform = OscillatorWaveform::Saw;
    osc.gain = 0.8F;
    osc.semitones = 0.0F;
    osc.cents = 0.0F;
    osc.pan = 0.0F;
    osc.stereoDivergence = 0.0F;
    preset.filter.cutoffHertz = 20000.0F;
    preset.filter.resonance = 0.0F;
    preset.filter.envelopeAmountOctaves = 0.0F;
    preset.filter.keyTrack = 0.0F;
    for (auto& slot : preset.modulation) slot.enabled = false;
    return preset;
}

std::vector<float> render_note_held(Synthesizer& synth, std::uint8_t note, std::size_t frames,
                                    float velocity = 1.0F) {
    std::vector<float> buffer(frames * 2U);
    require(synth.note_on(note, velocity), "note_on failed");
    synth.render(buffer.data(), frames);
    synth.all_notes_off(true);
    return buffer;
}

// 1. Analog pitch accuracy: 440 Hz in, 440 Hz out (within 1%).
void check_analog_pitch() {
    Synthesizer synth(48000);
    synth.set_preset(flat_single_saw());
    constexpr std::size_t frames = 48000;
    const auto buffer = render_note_held(synth, 69, frames);
    const AudioFeatureVector fv = extract_audio_features(buffer.data(), frames, 2, 48000.0);
    std::printf("[analog_pitch] measured %.2f Hz (confidence %.2f)\n",
                fv.estimatedPitchHz, fv.pitchConfidence);
    require(fv.pitchConfidence > 0.5, "analog_pitch: pitch confidence too low");
    require(std::abs(fv.estimatedPitchHz - 440.0) < 4.4, "analog_pitch: outside 1% tolerance");
    std::printf("[analog_pitch] PASS\n");
}

// 2. Wavetable frame continuity: sweeping the position across frame
// boundaries must not click. Smooth sine-family frames, so any large
// per-sample jump is a frame-boundary artifact, not the waveform itself.
void check_wavetable_continuity() {
    SynthPreset preset = flat_single_saw();
    preset.oscillators[0].waveform = OscillatorWaveform::Wavetable;
    preset.wavetable.name = "acceptance-smooth";
    preset.wavetable.frameCount = 4;
    for (std::size_t f = 0; f < 4; ++f) {
        for (std::size_t s = 0; s < kWavetableSampleCount; ++s) {
            const float phase = static_cast<float>(s) / static_cast<float>(kWavetableSampleCount);
            preset.wavetable.samples[f * kWavetableSampleCount + s] =
                std::sin(2.0F * 3.14159265F * (phase + 0.25F * static_cast<float>(f)));
        }
    }
    preset.lfos[0].enabled = true;
    preset.lfos[0].waveform = LfoWaveform::Triangle;
    preset.lfos[0].rateHertz = 0.5F;
    preset.lfos[0].depth = 1.0F;
    preset.modulation[0] = {true, ModulationSource::Lfo1, ModulationDestination::WavetablePosition,
                            0.5F, 0.5F, ModulationCurve::Linear, ModulationPolarity::Bipolar, 0.0F};

    constexpr std::size_t frames = 96000;  // 2 s @ 48 kHz
    std::vector<float> swept;
    {
        Synthesizer synth(48000);
        synth.set_preset(preset);
        swept = render_note_held(synth, 57, frames);  // 220 Hz: gentle waveform slope
    }
    // Sanity: the sweep must actually move the position (static != swept).
    {
        SynthPreset frozen = preset;
        frozen.modulation[0].enabled = false;
        frozen.oscillators[0].wavetablePosition = 0.0F;
        Synthesizer synth(48000);
        synth.set_preset(frozen);
        const auto still = render_note_held(synth, 57, frames);
        double diff = 0.0;
        for (std::size_t i = 0; i < swept.size(); ++i) diff += std::abs(swept[i] - still[i]);
        diff /= static_cast<double>(swept.size());
        std::printf("[wavetable_continuity] sweep-vs-static mean abs diff %.4f\n", diff);
        require(diff > 0.01, "wavetable_continuity: position sweep did not move");
    }

    float peak = 0.0F;
    for (float v : swept) peak = std::max(peak, std::abs(v));
    require(peak > 0.01F, "wavetable_continuity: render silent");
    // Per-channel consecutive-frame deltas (buffer is interleaved stereo).
    float maxDelta = 0.0F;
    for (std::size_t i = 0; i + 2 < swept.size(); i += 2)
        maxDelta = std::max(maxDelta, std::abs(swept[i + 2] - swept[i]));
    std::printf("[wavetable_continuity] peak %.3f max|delta| %.4f (%.1f%% of peak)\n",
                peak, maxDelta, 100.0 * maxDelta / peak);
    require(maxDelta < 0.10F * peak, "wavetable_continuity: frame-boundary click detected");
    std::printf("[wavetable_continuity] PASS\n");
}

// 3. Physics determinism: identical construction + identical preset with a
// physics modulation routing must render bit-identical output.
void check_physics_determinism() {
    SynthPreset preset = flat_single_saw();
    preset.modulation[0] = {true, ModulationSource::Spring, ModulationDestination::FilterCutoff,
                            0.8F, 0.0F, ModulationCurve::Linear, ModulationPolarity::Bipolar, 0.0F};
    constexpr std::size_t frames = 48000;
    std::vector<float> a(frames * 2U), b(frames * 2U);
    for (int pass = 0; pass < 2; ++pass) {
        Synthesizer synth(48000);
        synth.set_preset(preset);
        require(synth.note_on(69, 1.0F), "note_on failed");
        synth.render((pass == 0 ? a : b).data(), frames);
    }
    std::size_t diffs = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++diffs;
    std::printf("[physics_determinism] %zu differing samples of %zu\n", diffs, a.size());
    require(diffs == 0, "physics_determinism: renders are not bit-identical");
    std::printf("[physics_determinism] PASS\n");
}

// 4. Stereo/mono compatibility: full stereo divergence must keep the L/R
// Pearson correlation inside the plan's [0.1, 1.0] bounds.
void check_stereo_mono_compat() {
    SynthPreset preset = SynthPreset::make_default();
    for (auto& osc : preset.oscillators) osc.stereoDivergence = 1.0F;
    Synthesizer synth(48000);
    synth.set_preset(preset);
    constexpr std::size_t frames = 96000;
    const auto buffer = render_note_held(synth, 69, frames);
    double sumL = 0.0, sumR = 0.0, sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double l = buffer[2 * i], r = buffer[2 * i + 1];
        sumL += l; sumR += r; sumLL += l * l; sumRR += r * r; sumLR += l * r;
    }
    const double n = static_cast<double>(frames);
    const double cov = sumLR - sumL * sumR / n;
    const double varL = sumLL - sumL * sumL / n;
    const double varR = sumRR - sumR * sumR / n;
    require(varL > 0.0 && varR > 0.0, "stereo_mono_compat: silent channel");
    const double corr = cov / std::sqrt(varL * varR);
    std::printf("[stereo_mono_compat] L/R correlation %.4f\n", corr);
    require(corr >= 0.1 && corr <= 1.0, "stereo_mono_compat: correlation outside [0.1, 1.0]");
    std::printf("[stereo_mono_compat] PASS\n");
}

// 5. Morph robustness: every interpolated preset validates and renders
// finite audio.
void check_morph_no_nan() {
    const auto bank = SynthPreset::builtin_presets();
    require(bank.size() > 3, "morph_no_nan: preset bank too small");
    const SynthPreset& a = bank[0];
    const SynthPreset& b = bank[3];
    std::printf("[morph_no_nan] morphing \"%s\" -> \"%s\"\n", a.name.c_str(), b.name.c_str());
    Synthesizer synth(48000);
    constexpr std::size_t frames = 12000;  // 0.25 s per step
    std::vector<float> buffer(frames * 2U);
    int steps = 0;
    for (int t = 0; t <= 50; ++t) {
        const float amount = static_cast<float>(t) / 50.0F;
        SynthPreset m = morph_synth_presets(a, b, amount);
        std::string error;
        require(m.validate(&error),
                "morph_no_nan: invalid preset at t=" + std::to_string(amount) + ": " + error);
        synth.set_preset(m);
        require(synth.note_on(69, 1.0F), "note_on failed");
        synth.render(buffer.data(), frames);
        synth.all_notes_off(true);
        for (float s : buffer)
            require(std::isfinite(s),
                    "morph_no_nan: non-finite sample at t=" + std::to_string(amount));
        ++steps;
    }
    std::printf("[morph_no_nan] %d steps, all valid and finite: PASS\n", steps);
}

// 6. MPE note isolation: a bend on member channel 1 must not leak to
// member channel 2.
void check_mpe_note_isolation() {
    SynthPreset preset = SynthPreset::make_default();
    preset.mpe.zoneMode = MpeZoneMode::Lower;
    preset.mpe.lowerMasterChannel = 0;
    preset.mpe.lowerMemberCount = 15;
    preset.mpe.masterPitchBendRangeSemitones = 2.0F;
    preset.mpe.memberPitchBendRangeSemitones = 48.0F;
    Synthesizer synth(48000);
    synth.set_preset(preset);
    require(synth.note_on(60, 1.0F, 1), "note_on ch1 failed");
    require(synth.note_on(64, 1.0F, 2), "note_on ch2 failed");
    require(synth.pitch_bend(4096, 1), "pitch_bend ch1 failed");  // +24 semitones at 48-st range
    std::vector<float> buffer(1024 * 2U);
    synth.render(buffer.data(), 1024);
    const auto voices = synth.voices();
    const SynthVoiceInfo* v1 = nullptr;
    const SynthVoiceInfo* v2 = nullptr;
    for (const auto& v : voices) {
        if (v.active && v.channel == 1 && v.note == 60) v1 = &v;
        if (v.active && v.channel == 2 && v.note == 64) v2 = &v;
    }
    require(v1 && v2, "mpe_note_isolation: voices not allocated");
    std::printf("[mpe_note_isolation] ch1 bend %.2f st, ch2 bend %.2f st\n",
                v1->pitchBendSemitones, v2->pitchBendSemitones);
    require(std::abs(v1->pitchBendSemitones - 24.0F) < 1.0F, "mpe_note_isolation: ch1 bend wrong");
    require(std::abs(v2->pitchBendSemitones) < 0.5F, "mpe_note_isolation: bend leaked to ch2");
    std::printf("[mpe_note_isolation] PASS\n");
}

// 7. Polyphony stress: max polyphony (16 voices — the engine's voice count)
// held for 30 s; every 512-frame block must render inside its real-time
// budget. (The plan's 48-voice figure predates the 16-voice engine; the
// gate is the per-block deadline, which is what "no deadline misses" means.)
void check_polyphony_stress() {
    SynthPreset preset = SynthPreset::make_default();
    for (auto& osc : preset.oscillators) {
        osc.enabled = true;
        osc.gain = 0.15F;
        osc.stereoDivergence = 1.0F;
    }
    preset.oscillators[0].waveform = OscillatorWaveform::SuperSaw;
    preset.oscillators[6].waveform = OscillatorWaveform::SuperSaw;
    Synthesizer synth(48000);
    synth.set_preset(preset);
    for (std::uint8_t n = 0; n < 16; ++n)
        require(synth.note_on(static_cast<std::uint8_t>(36 + n), 1.0F), "note_on failed");
    constexpr std::size_t blockFrames = 512;
    constexpr std::size_t blocks = (48000U * 30U) / blockFrames;
    const double budgetMs = 1000.0 * static_cast<double>(blockFrames) / 48000.0;
    std::vector<float> block(blockFrames * 2U);
    for (int i = 0; i < 8; ++i) synth.render(block.data(), blockFrames);  // warmup, untimed
    double worstMs = 0.0;
    std::size_t misses = 0;
    for (std::size_t i = 0; i < blocks; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        synth.render(block.data(), blockFrames);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        worstMs = std::max(worstMs, ms);
        if (ms >= budgetMs) ++misses;
    }
    synth.all_notes_off(true);
    std::printf("[polyphony_stress] 16 voices x 30 s: worst block %.2f ms (budget %.2f ms), %zu misses\n",
                worstMs, budgetMs, misses);
    require(misses == 0, "polyphony_stress: deadline misses");
    std::printf("[polyphony_stress] PASS\n");
}

}  // namespace

int main(int argc, char** argv) {
    // Optional argv[1] selects a single check (e.g. for isolating a failure);
    // default runs the full acceptance sequence in order.
    const std::string only = argc > 1 ? argv[1] : "";
    const auto want = [&](const char* name) { return only.empty() || only == name; };
    try {
        if (want("analog_pitch")) check_analog_pitch();
        if (want("wavetable_continuity")) check_wavetable_continuity();
        if (want("physics_determinism")) check_physics_determinism();
        if (want("stereo_mono_compat")) check_stereo_mono_compat();
        if (want("morph_no_nan")) check_morph_no_nan();
        if (want("mpe_note_isolation")) check_mpe_note_isolation();
        if (want("polyphony_stress")) check_polyphony_stress();
        std::printf("dve_synth_release_acceptance_tests: PASS\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("dve_synth_release_acceptance_tests: FAIL: %s\n", e.what());
        return 1;
    }
}
