#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <vector>

#include "dve/audio/mixer.hpp"

int main() {
    using namespace dve::audio;
    constexpr std::uint32_t sampleRate = 48000U;
    constexpr std::size_t blockFrames = 256U;
    constexpr std::size_t iterations = 2000U;
    AudioMixer mixer(sampleRate);
    ResidentSampleDesc sample;
    sample.name = "dense debris bed";
    sample.sampleRate = sampleRate;
    sample.channels = 1U;
    sample.samples.resize(sampleRate / 2U);
    std::uint32_t noise = 0x12345678U;
    for (std::size_t i = 0; i < sample.samples.size(); ++i) {
        noise ^= noise << 13U; noise ^= noise >> 17U; noise ^= noise << 5U;
        const float n = static_cast<float>(static_cast<int>(noise & 65535U) - 32768) / 32768.0F;
        sample.samples[i] = n * std::exp(-6.0F * static_cast<float>(i) / static_cast<float>(sample.samples.size())) * 0.08F;
    }
    std::string error;
    const SampleId sampleId = mixer.register_resident_sample(std::move(sample), &error);
    if (!sampleId) { std::cerr << error << '\n'; return 1; }
    for (std::size_t i = 0; i < kMaxLogicalSampleVoices; ++i) {
        PlaySampleDesc desc;
        desc.sample = sampleId;
        desc.loop = true;
        desc.gain = 0.12F;
        desc.priority = i < 12U ? AudioPriority::Hero : AudioPriority::Background;
        desc.emitter.position = {static_cast<float>(static_cast<int>(i % 16U) - 8),
                                 static_cast<float>(i % 3U),
                                 -2.0F - static_cast<float>(i / 16U)};
        (void)mixer.play_sample(desc);
    }
    for (std::uint8_t note = 48U; note < 64U; ++note) (void)mixer.synthesizer().note_on(note, 0.45F);
    std::vector<float> output(blockFrames * 2U);
    mixer.render(output);

    std::vector<double> milliseconds;
    milliseconds.reserve(iterations);
    const auto totalStart = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        mixer.render(output);
        const auto end = std::chrono::steady_clock::now();
        milliseconds.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    const auto totalEnd = std::chrono::steady_clock::now();
    std::sort(milliseconds.begin(), milliseconds.end());
    const double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
    double sum{};
    for (double value : milliseconds) sum += value;
    auto percentile = [&](double p) {
        return milliseconds[std::min(milliseconds.size() - 1U,
                                     static_cast<std::size_t>(p * static_cast<double>(milliseconds.size() - 1U)))];
    };
    const double blockDeadlineMs = static_cast<double>(blockFrames) * 1000.0 / static_cast<double>(sampleRate);
    const double renderedSeconds = static_cast<double>(blockFrames * iterations) / static_cast<double>(sampleRate);
    const AudioMixerMeters meters = mixer.meters();
    std::cout << std::fixed << std::setprecision(6)
              << "{\n"
              << "  \"sample_rate\": " << sampleRate << ",\n"
              << "  \"block_frames\": " << blockFrames << ",\n"
              << "  \"iterations\": " << iterations << ",\n"
              << "  \"logical_sample_voices\": " << meters.logicalSampleVoices << ",\n"
              << "  \"physical_sample_voices\": " << meters.physicalSampleVoices << ",\n"
              << "  \"virtual_sample_voices\": " << meters.virtualSampleVoices << ",\n"
              << "  \"synth_voices\": " << meters.synthVoices << ",\n"
              << "  \"callback_mean_ms\": " << sum / static_cast<double>(milliseconds.size()) << ",\n"
              << "  \"callback_p50_ms\": " << percentile(0.50) << ",\n"
              << "  \"callback_p95_ms\": " << percentile(0.95) << ",\n"
              << "  \"callback_p99_ms\": " << percentile(0.99) << ",\n"
              << "  \"callback_max_ms\": " << milliseconds.back() << ",\n"
              << "  \"block_deadline_ms\": " << blockDeadlineMs << ",\n"
              << "  \"p99_deadline_fraction\": " << percentile(0.99) / blockDeadlineMs << ",\n"
              << "  \"offline_realtime_factor\": " << renderedSeconds / (totalMs / 1000.0) << "\n"
              << "}\n";
    return 0;
}
