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

#include "dve/audio/sample_map.hpp"
#include "dve/audio/synthesizer.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using namespace dve::audio;
constexpr float kPi = 3.14159265358979323846F;
constexpr std::uint32_t kSourceFrames = 12000U;

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
    SynthGranularProfiler profiler{};
};

SynthSampleMap make_sample_map() {
    SynthSampleMap map;
    map.name = "DVE v1.27 Production Benchmark Map";
    map.sourceCount = 2U;
    map.zoneCount = 2U;
    map.residentFrameCount = kSourceFrames * 2U;
    for (std::uint32_t sourceIndex = 0U; sourceIndex < 2U; ++sourceIndex) {
        auto& source = map.sources[sourceIndex];
        source.name = sourceIndex == 0U ? "Glass A" : "Glass B";
        source.preload = SamplePreloadPolicy::Resident;
        source.sampleRate = 48000U;
        source.totalFrameCount = kSourceFrames;
        source.residentFrameOffset = sourceIndex * kSourceFrames;
        source.residentFrameCount = kSourceFrames;
    }
    for (std::uint32_t i = 0; i < kSourceFrames; ++i) {
        const float t = static_cast<float>(i) / 48000.0F;
        const float envelope = std::exp(-2.1F * t);
        map.residentSamples[i] = envelope *
            (0.52F * std::sin(2.0F * kPi * 196.0F * t) +
             0.24F * std::sin(2.0F * kPi * 392.0F * t + 0.4F) +
             0.13F * std::sin(2.0F * kPi * 783.7F * t + 1.1F));
        map.residentSamples[kSourceFrames + i] = envelope *
            (0.48F * std::sin(2.0F * kPi * 196.0F * t + 0.31F) +
             0.26F * std::sin(2.0F * kPi * 588.0F * t + 0.8F) +
             0.11F * std::sin(2.0F * kPi * 980.0F * t + 1.7F));
    }
    for (std::uint8_t index = 0U; index < 2U; ++index) {
        auto& zone = map.zones[index];
        zone.sourceIndex = index;
        zone.keyLow = 0U;
        zone.keyHigh = 127U;
        zone.velocityLow = 1U;
        zone.velocityHigh = 127U;
        zone.rootNote = 55U;
        zone.roundRobinGroup = 1U;
        zone.roundRobinIndex = index;
        zone.roundRobinCount = 2U;
        zone.startFrame = 0U;
        zone.endFrame = kSourceFrames;
        zone.loopMode = SampleLoopMode::Forward;
        zone.loopStartFrame = 1024U;
        zone.loopEndFrame = 10240U;
        zone.loopCrossfadeFrames = 256U;
        zone.pan = index == 0U ? -0.06F : 0.06F;
    }
    return map;
}

SynthPreset make_preset() {
    auto preset = SynthPreset::make_default();
    preset.name = "DVE v1.27 Sample Map + Granular Performance Patch";
    preset.masterGain = 0.62F;
    preset.sampleBank = {};
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;

    auto& grain = preset.oscillators[0];
    grain.enabled = true;
    grain.waveform = OscillatorWaveform::Granular;
    grain.gain = 0.42F;
    grain.grainPosition = 0.31F;
    grain.grainSizeMilliseconds = 88.0F;
    grain.grainDensityHertz = 31.0F;
    grain.grainSpray = 0.28F;
    grain.grainStereoSpread = 0.92F;
    grain.grainStereoMotion = 0.52F;
    grain.grainEnvelopeCurve = 1.65F;
    grain.grainReverseProbability = 0.10F;
    grain.grainPitchRandomSemitones = 7.0F;
    grain.grainPitchQuantizeSemitones = 7.0F;
    grain.grainDensityVelocity = 0.25F;
    grain.grainDensityTimbre = 0.20F;
    grain.grainWindow = GrainWindow::Hann;

    auto& sample = preset.oscillators[1];
    sample.enabled = true;
    sample.waveform = OscillatorWaveform::Sample;
    sample.gain = 0.18F;
    sample.semitones = -12.0F;
    sample.sampleStart = 0.02F;
    sample.sampleEnd = 0.96F;
    sample.sampleLoop = true;
    sample.sampleKeyTrack = true;

    preset.filter.topology = FilterTopology::OberheimSem;
    preset.filter.cutoffHertz = 5200.0F;
    preset.filter.resonance = 0.22F;
    preset.filter.drive = 1.1F;
    preset.chorus.enabled = true;
    preset.reverb.enabled = true;
    preset.compressor.enabled = true;
    preset.limiter.enabled = true;
    return preset;
}

Result run(std::size_t blockFrames, double seconds) {
    constexpr std::uint32_t sampleRate = 48000U;
    Synthesizer synth(sampleRate);
    synth.set_preset(make_preset());
    std::string error;
    if (!synth.set_sample_map(make_sample_map(), &error))
        throw std::runtime_error("sample map setup failed: " + error);

    constexpr std::array<std::uint8_t, 12> notes{48, 52, 55, 59, 60, 64, 67, 71, 72, 76, 79, 83};
    for (std::size_t i = 0; i < notes.size(); ++i)
        (void)synth.note_on(notes[i], 0.54F + static_cast<float>(i % 4U) * 0.08F,
                            static_cast<std::uint8_t>(i % 4U));

    std::vector<float> block(blockFrames * 2U);
    const std::size_t warmupBlocks = (sampleRate / blockFrames) * 2U;
    for (std::size_t i = 0; i < warmupBlocks; ++i) synth.render(block);
    synth.reset_granular_profiler();

    const std::size_t totalFrames = static_cast<std::size_t>(std::ceil(seconds * sampleRate));
    const std::size_t blockCount = (totalFrames + blockFrames - 1U) / blockFrames;
    std::vector<double> durations;
    durations.reserve(blockCount);
    const auto wallStart = Clock::now();
    for (std::size_t i = 0; i < blockCount; ++i) {
        const auto start = Clock::now();
        synth.render(block);
        durations.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    const double wallSeconds = std::chrono::duration<double>(Clock::now() - wallStart).count();
    std::sort(durations.begin(), durations.end());
    return {blockFrames,
            static_cast<double>(blockCount * blockFrames) / sampleRate,
            wallSeconds,
            std::accumulate(durations.begin(), durations.end(), 0.0) / static_cast<double>(durations.size()),
            percentile(durations, 0.95), percentile(durations, 0.99), durations.back(),
            synth.granular_profiler()};
}

void write_json(std::ostream& out, const std::array<Result, 3>& results) {
    out << std::fixed << std::setprecision(4);
    out << "{\n  \"benchmark\": \"dve_v1_27_production_sample_map_granular\",\n"
        << "  \"sample_rate_hz\": 48000,\n"
        << "  \"voices\": 12,\n"
        << "  \"sample_oscillators_per_voice\": 1,\n"
        << "  \"granular_oscillators_per_voice\": 1,\n"
        << "  \"grain_capacity_per_oscillator\": 8,\n"
        << "  \"sample_map_sources\": 2,\n"
        << "  \"sample_map_zones\": 2,\n"
        << "  \"resident_frames\": " << kSourceFrames * 2U << ",\n"
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
            << "      \"p99_deadline_fraction\": " << r.p99Micros / deadline << ",\n"
            << "      \"requested_grains\": " << r.profiler.requestedGrains << ",\n"
            << "      \"admitted_grains\": " << r.profiler.admittedGrains << ",\n"
            << "      \"grain_steals\": " << r.profiler.grainSteals << ",\n"
            << "      \"grain_misses\": " << r.profiler.grainMisses << ",\n"
            << "      \"maximum_active_grains\": " << r.profiler.maximumActiveGrains << ",\n"
            << "      \"page_underruns\": " << r.profiler.pageUnderruns << "\n"
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
            std::cerr << "usage: dve_audio_synth_sample_granular_bench [--seconds N] [--json PATH]\n";
            return 2;
        }
    }
    try {
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
    } catch (const std::exception& exception) {
        std::cerr << "benchmark failed: " << exception.what() << '\n';
        return 1;
    }
}
