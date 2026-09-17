#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

namespace {
using Clock = std::chrono::steady_clock;

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    const double position = fraction * static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double t = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - t) + sorted[upper] * t;
}

struct Result {
    std::size_t blockFrames{};
    double renderedSeconds{};
    double wallSeconds{};
    double meanMicros{};
    double p95Micros{};
    double p99Micros{};
    double maxMicros{};
};

Result run(std::size_t blockFrames, double seconds) {
    constexpr std::uint32_t sampleRate = 48000U;
    dve::audio::Synthesizer synth(sampleRate);
    auto preset = dve::audio::SynthPreset::make_default();
    preset.oscillatorQuality = dve::audio::OscillatorQuality::Normal;
    preset.filterQuality = dve::audio::FilterQuality::Standard;
    preset.unison = {true, 2U, 8.0F, 0.55F, 0.12F, true};
    preset.name = "DVE Eightfold Modular Typical Poly Patch";
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    preset.oscillators[0].enabled = true;
    preset.oscillators[0].waveform = dve::audio::OscillatorWaveform::Saw;
    preset.oscillators[0].gain = 0.16F;
    preset.oscillators[0].pan = -0.24F;
    preset.oscillators[1].enabled = true;
    preset.oscillators[1].waveform = dve::audio::OscillatorWaveform::Pulse;
    preset.oscillators[1].gain = 0.13F;
    preset.oscillators[1].pan = 0.24F;
    preset.oscillators[1].pwmDepth = 0.42F;
    preset.oscillators[1].pwmRateHertz = 2.1F;
    preset.oscillators[2].enabled = true;
    preset.oscillators[2].waveform = dve::audio::OscillatorWaveform::Triangle;
    preset.oscillators[2].gain = 0.09F;
    preset.oscillators[2].semitones = -12.0F;
    preset.oscillators[3].enabled = true;
    preset.oscillators[3].waveform = dve::audio::OscillatorWaveform::Wavetable;
    preset.oscillators[3].gain = 0.11F;
    preset.oscillators[3].wavetablePosition = 0.38F;
    preset.wavetable.enabled = true;
    preset.lfos[0] = {true, dve::audio::LfoWaveform::Triangle, 2.1F, 0.75F, 0.0F, 0.04F, true, false, 1.0F};
    preset.modulation[0] = {true, dve::audio::ModulationSource::Lfo1,
        dve::audio::ModulationDestination::Osc2PulseWidth, 0.65F,
        dve::audio::ModulationCurve::Linear};
    preset.modulation[1] = {true, dve::audio::ModulationSource::Velocity,
        dve::audio::ModulationDestination::FilterCutoff, 0.25F,
        dve::audio::ModulationCurve::Quadratic};
    preset.filter.topology = dve::audio::FilterTopology::MoogLadder;
    preset.filter.cutoffHertz = 2400.0F;
    preset.filter.resonance = 0.38F;
    preset.filter.drive = 1.55F;
    preset.filter.oversampling = dve::audio::FilterOversampling::X1;
    preset.distortion.enabled = false;
    preset.eq.enabled = false;
    preset.chorus.enabled = true;
    preset.phaser.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = true;
    preset.compressor.enabled = true;
    preset.limiter.enabled = true;
    preset.masterGain = 0.55F;
    synth.set_preset(preset);

    constexpr std::array<std::uint8_t, 8> notes{48, 52, 55, 59, 60, 64, 67, 71};
    for (std::size_t i = 0; i < notes.size(); ++i)
        (void)synth.note_on(notes[i], 0.62F + static_cast<float>(i % 3U) * 0.07F,
                            static_cast<std::uint8_t>(i % 2U));

    std::vector<float> block(blockFrames * 2U);
    const std::size_t warmupBlocks = (sampleRate / blockFrames) * 2U;
    for (std::size_t i = 0; i < warmupBlocks; ++i) synth.render(block);

    const std::size_t totalFrames = static_cast<std::size_t>(std::ceil(seconds * sampleRate));
    const std::size_t blockCount = (totalFrames + blockFrames - 1U) / blockFrames;
    std::vector<double> durations;
    durations.reserve(blockCount);
    const auto wallStart = Clock::now();
    for (std::size_t i = 0; i < blockCount; ++i) {
        const auto start = Clock::now();
        synth.render(block);
        const auto end = Clock::now();
        durations.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    const double wallSeconds = std::chrono::duration<double>(Clock::now() - wallStart).count();
    std::sort(durations.begin(), durations.end());
    return {blockFrames,
            static_cast<double>(blockCount * blockFrames) / sampleRate,
            wallSeconds,
            std::accumulate(durations.begin(), durations.end(), 0.0) / static_cast<double>(durations.size()),
            percentile(durations, 0.95), percentile(durations, 0.99), durations.back()};
}

void write_json(std::ostream& out, const std::array<Result, 3>& results) {
    out << std::fixed << std::setprecision(4);
    out << "{\n  \"benchmark\": \"dve_audio_synth_expressive_typical_poly_patch\",\n"
        << "  \"sample_rate_hz\": 48000,\n"
        << "  \"voices\": 8,\n"
        << "  \"oscillators_per_voice\": 4,\n"
        << "  \"modulation_slots_active\": 2,\n"
        << "  \"filter_oversampling\": 1,\n"
        << "  \"effects\": [\"chorus\", \"reverb\", \"compressor\", \"limiter\"],\n"
        << "  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const double deadline = static_cast<double>(r.blockFrames) / 48000.0 * 1.0e6;
        out << "    {\n"
            << "      \"block_frames\": " << r.blockFrames << ",\n"
            << "      \"rendered_seconds\": " << r.renderedSeconds << ",\n"
            << "      \"wall_seconds\": " << r.wallSeconds << ",\n"
            << "      \"estimated_single_core_percent\": " << r.wallSeconds / r.renderedSeconds * 100.0 << ",\n"
            << "      \"callback_mean_us\": " << r.meanMicros << ",\n"
            << "      \"callback_p95_us\": " << r.p95Micros << ",\n"
            << "      \"callback_p99_us\": " << r.p99Micros << ",\n"
            << "      \"callback_max_us\": " << r.maxMicros << ",\n"
            << "      \"callback_deadline_us\": " << deadline << ",\n"
            << "      \"p99_deadline_fraction\": " << r.p99Micros / deadline << "\n"
            << "    }" << (i + 1U == results.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}
} // namespace

int main(int argc, char** argv) {
    std::filesystem::path output;
    double seconds = 4.0;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--json" && i + 1 < argc) output = argv[++i];
        else if (argument == "--seconds" && i + 1 < argc) seconds = std::max(1.0, std::stod(argv[++i]));
        else {
            std::cerr << "usage: dve_audio_synth_typical_bench [--seconds N] [--json PATH]\n";
            return 2;
        }
    }
    const std::array<std::size_t, 3> blockSizes{128U, 256U, 512U};
    std::array<Result, 3> results{};
    for (std::size_t i = 0; i < blockSizes.size(); ++i) results[i] = run(blockSizes[i], seconds);
    write_json(std::cout, results);
    if (!output.empty()) {
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file) return 1;
        write_json(file, results);
    }
    return 0;
}
