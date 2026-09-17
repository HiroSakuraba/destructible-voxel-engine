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
#include <span>
#include <string>
#include <vector>

#include "dve/audio/synthesizer.hpp"

namespace {
using Clock = std::chrono::steady_clock;

struct Result {
    std::size_t blockFrames{};
    double renderedSeconds{};
    double wallSeconds{};
    double realtimeFactor{};
    double singleCorePercent{};
    double meanMicros{};
    double p50Micros{};
    double p95Micros{};
    double p99Micros{};
    double maxMicros{};
    double callbackDeadlineMicros{};
    std::uint32_t activeVoices{};
    float peakLeft{};
    float peakRight{};
};

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    const double position = fraction * static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double t = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - t) + sorted[upper] * t;
}

Result run_benchmark(std::size_t blockFrames, double seconds) {
    constexpr std::uint32_t sampleRate = 48000U;
    dve::audio::Synthesizer synth(sampleRate);
    auto preset = dve::audio::SynthPreset::make_default();
    preset.name = "DVE Eightfold Expressive Maximum Load";
    preset.oscillatorQuality = dve::audio::OscillatorQuality::High;
    preset.filterQuality = dve::audio::FilterQuality::High;
    preset.unison = {false, 1U, 12.0F, 0.75F, 0.2F, true};
    preset.mpe.zoneMode = dve::audio::MpeZoneMode::Lower;
    static constexpr std::array waveforms{
        dve::audio::OscillatorWaveform::Wavetable, dve::audio::OscillatorWaveform::SuperSaw,
        dve::audio::OscillatorWaveform::Pulse, dve::audio::OscillatorWaveform::Organ,
        dve::audio::OscillatorWaveform::FoldedSine, dve::audio::OscillatorWaveform::Digital,
        dve::audio::OscillatorWaveform::Saw, dve::audio::OscillatorWaveform::Triangle};
    preset.wavetable.enabled = true;
    for (std::size_t i = 0; i < preset.oscillators.size(); ++i) {
        auto& oscillator = preset.oscillators[i];
        oscillator.enabled = true;
        oscillator.waveform = waveforms[i];
        oscillator.gain = 0.060F;
        oscillator.pan = (i % 2U == 0U) ? -0.22F : 0.22F;
        oscillator.shape = 0.18F + static_cast<float>(i) * 0.09F;
        oscillator.wavetablePosition = static_cast<float>(i) / 7.0F;
        oscillator.pwmDepth = i == 2U ? 0.72F : 0.0F;
        oscillator.pwmRateHertz = 3.7F;
    }
    preset.oscillators[1].hardSyncSource = 0;
    preset.oscillators[3].frequencyModSource = 0;
    preset.oscillators[3].frequencyModMode = dve::audio::FrequencyModulationMode::Exponential;
    preset.oscillators[3].frequencyModAmount = 0.32F;
    preset.oscillators[5].ringModSource = 4;
    preset.oscillators[5].ringModDepth = 0.45F;
    preset.oscillators[6].subOscillatorLevel = 0.35F;
    preset.lfos[0] = {true, dve::audio::LfoWaveform::Triangle, 5.1F, 1.0F, 0.0F, 0.04F, true, false, 1.0F};
    preset.lfos[1] = {true, dve::audio::LfoWaveform::SmoothRandom, 0.9F, 1.0F, 0.23F, 0.0F, true, false, 1.0F};
    preset.modulation[0] = {true, dve::audio::ModulationSource::Lfo1, dve::audio::ModulationDestination::Osc3PulseWidth, 0.75F, dve::audio::ModulationCurve::Linear};
    preset.modulation[1] = {true, dve::audio::ModulationSource::Lfo2, dve::audio::ModulationDestination::FilterCutoff, 0.48F, dve::audio::ModulationCurve::Cubic};
    preset.modulation[2] = {true, dve::audio::ModulationSource::Velocity, dve::audio::ModulationDestination::FilterDrive, 0.35F, dve::audio::ModulationCurve::Quadratic};
    preset.modulation[3] = {true, dve::audio::ModulationSource::Aftertouch, dve::audio::ModulationDestination::VoiceGain, 0.20F, dve::audio::ModulationCurve::Linear};
    preset.modulation[4] = {true, dve::audio::ModulationSource::Macro1, dve::audio::ModulationDestination::Osc1Shape, 0.60F, dve::audio::ModulationCurve::Linear};
    preset.macros.values[0] = 0.62F;
    preset.filter.topology = dve::audio::FilterTopology::KorgMs20;
    preset.filter.cutoffHertz = 2100.0F;
    preset.filter.ms20HighPassCutoffHertz = 75.0F;
    preset.filter.resonance = 0.72F;
    preset.filter.drive = 2.8F;
    preset.filter.oversampling = dve::audio::FilterOversampling::X2;
    preset.distortion.enabled = true;
    preset.eq.enabled = true;
    preset.chorus.enabled = true;
    preset.phaser.enabled = true;
    preset.delay.enabled = true;
    preset.reverb.enabled = true;
    preset.compressor.enabled = true;
    preset.limiter.enabled = true;
    preset.masterGain = 0.58F;
    synth.set_preset(preset);

    constexpr std::array<std::uint8_t, 16> chord{
        36, 40, 43, 47, 48, 52, 55, 59, 60, 64, 67, 71, 72, 76, 79, 83};
    for (std::size_t i = 0; i < chord.size(); ++i)
        (void)synth.note_on(chord[i], 0.58F + static_cast<float>(i % 4U) * 0.06F,
                            static_cast<std::uint8_t>(i % 4U));

    std::vector<float> block(blockFrames * 2U);
    const std::size_t warmupBlocks = (sampleRate / blockFrames) * 2U;
    for (std::size_t i = 0; i < warmupBlocks; ++i) synth.render(block);

    const std::size_t totalFrames = static_cast<std::size_t>(std::ceil(seconds * sampleRate));
    const std::size_t blockCount = (totalFrames + blockFrames - 1U) / blockFrames;
    std::vector<double> durations;
    durations.reserve(blockCount);

    const auto wallStart = Clock::now();
    for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        // Periodic modulation exercises MIDI parsing and parameter smoothing without creating
        // an unrealistic event storm.
        if ((blockIndex % 64U) == 0U) {
            const std::uint8_t value = static_cast<std::uint8_t>((blockIndex / 64U * 17U) % 128U);
            (void)synth.control_change(1U, value);
            (void)synth.control_change(74U, static_cast<std::uint8_t>(127U - value));
            (void)synth.pitch_bend(static_cast<std::int16_t>(static_cast<int>(value) * 96 - 6096));
        }
        const auto start = Clock::now();
        synth.render(block);
        const auto end = Clock::now();
        durations.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    const auto wallEnd = Clock::now();

    const double wallSeconds = std::chrono::duration<double>(wallEnd - wallStart).count();
    const double renderedSeconds = static_cast<double>(blockCount * blockFrames) / sampleRate;
    std::sort(durations.begin(), durations.end());
    const double mean = std::accumulate(durations.begin(), durations.end(), 0.0) /
                        static_cast<double>(durations.size());
    const auto meters = synth.meters();
    return {
        blockFrames,
        renderedSeconds,
        wallSeconds,
        renderedSeconds / std::max(wallSeconds, 1.0e-12),
        wallSeconds / renderedSeconds * 100.0,
        mean,
        percentile(durations, 0.50),
        percentile(durations, 0.95),
        percentile(durations, 0.99),
        durations.back(),
        static_cast<double>(blockFrames) / sampleRate * 1.0e6,
        meters.activeVoices,
        meters.peakLeft,
        meters.peakRight};
}

void write_json(std::ostream& out, std::span<const Result> results) {
    out << std::fixed << std::setprecision(4);
    out << "{\n  \"benchmark\": \"dve_audio_synth_expressive_maximum_load\",\n"
        << "  \"sample_rate_hz\": 48000,\n"
        << "  \"oscillators_per_voice\": 8,\n"
        << "  \"voices\": 16,\n"
        << "  \"modulation_slots_active\": 5,\n"
        << "  \"filter_oversampling\": 2,\n"
        << "  \"oscillator_quality\": \"high\",\n"
        << "  \"filter_quality\": \"high\",\n"
        << "  \"unison_voices\": 1,\n"
        << "  \"advanced_oscillator_paths\": [\"wavetable\", \"hard_sync\", \"exponential_fm\", \"ring_mod\", \"sub_oscillator\"],\n"
        << "  \"enabled_effects\": [\"distortion\", \"eq\", \"chorus\", \"phaser\", \"delay\", \"reverb\", \"compressor\", \"limiter\"],\n"
        << "  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\n"
            << "      \"block_frames\": " << r.blockFrames << ",\n"
            << "      \"rendered_seconds\": " << r.renderedSeconds << ",\n"
            << "      \"wall_seconds\": " << r.wallSeconds << ",\n"
            << "      \"realtime_factor\": " << r.realtimeFactor << ",\n"
            << "      \"estimated_single_core_percent\": " << r.singleCorePercent << ",\n"
            << "      \"callback_mean_us\": " << r.meanMicros << ",\n"
            << "      \"callback_p50_us\": " << r.p50Micros << ",\n"
            << "      \"callback_p95_us\": " << r.p95Micros << ",\n"
            << "      \"callback_p99_us\": " << r.p99Micros << ",\n"
            << "      \"callback_max_us\": " << r.maxMicros << ",\n"
            << "      \"callback_deadline_us\": " << r.callbackDeadlineMicros << ",\n"
            << "      \"p99_deadline_fraction\": " << r.p99Micros / r.callbackDeadlineMicros << ",\n"
            << "      \"active_voices\": " << r.activeVoices << ",\n"
            << "      \"peak_left\": " << r.peakLeft << ",\n"
            << "      \"peak_right\": " << r.peakRight << "\n"
            << "    }" << (i + 1U == results.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}
} // namespace

int main(int argc, char** argv) {
    std::filesystem::path output;
    double seconds = 6.0;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--json" && i + 1 < argc) output = argv[++i];
        else if (argument == "--seconds" && i + 1 < argc) seconds = std::max(1.0, std::stod(argv[++i]));
        else {
            std::cerr << "usage: dve_audio_synth_bench [--seconds N] [--json PATH]\n";
            return 2;
        }
    }

    const std::array<std::size_t, 3> blockSizes{128U, 256U, 512U};
    std::array<Result, blockSizes.size()> results{};
    for (std::size_t i = 0; i < blockSizes.size(); ++i) results[i] = run_benchmark(blockSizes[i], seconds);

    write_json(std::cout, results);
    if (!output.empty()) {
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "could not create benchmark output: " << output << '\n';
            return 1;
        }
        write_json(file, results);
    }
    return 0;
}
