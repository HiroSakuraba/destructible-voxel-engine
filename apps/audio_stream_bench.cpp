#include "dve/audio/mixer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <thread>
#include <vector>

int main() {
    using namespace dve::audio;
    constexpr std::uint32_t sampleRate = 48000U;
    constexpr std::size_t blockFrames = 256U;
    constexpr std::size_t iterations = 100U;
    constexpr std::size_t streamCount = 4U;

    const auto path = std::filesystem::temp_directory_path() / "dve_audio_stream_bench.dvesample";
    DecodedAudioAsset source;
    source.metadata.name = "four-stream benchmark";
    source.metadata.sampleRate = sampleRate;
    source.metadata.channels = 2U;
    source.metadata.storagePolicy = AudioStoragePolicy::Streamed;
    source.metadata.loop = {0U, sampleRate * 2U, true};
    source.samples.resize(static_cast<std::size_t>(sampleRate) * 2U * 2U);
    for (std::size_t frame = 0; frame < source.samples.size() / 2U; ++frame) {
        const float t = static_cast<float>(frame) / static_cast<float>(sampleRate);
        source.samples[frame * 2U] = std::sin(2.0F * std::numbers::pi_v<float> * 110.0F * t) * 0.04F;
        source.samples[frame * 2U + 1U] = std::sin(2.0F * std::numbers::pi_v<float> * 165.0F * t) * 0.04F;
    }
    source.metadata.frameCount = source.samples.size() / 2U;
    std::string error;
    if (!write_cooked_audio_asset(path, source, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    AudioMixer mixer(sampleRate);
    std::array<StreamSampleId, streamCount> streams{};
    for (std::size_t i = 0; i < streams.size(); ++i) {
        streams[i] = mixer.register_streamed_sample(path, 32768U, &error);
        if (!streams[i]) {
            std::cerr << error << '\n';
            return 1;
        }
        PlayStreamDesc play;
        play.sample = streams[i];
        play.bus = i < 2U ? AudioBusId::Music : AudioBusId::Ambience;
        play.gain = 0.25F;
        play.loop = true;
        (void)mixer.play_stream(play);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::array<float, blockFrames * 2U> output{};
    mixer.render(output);
    std::vector<double> times;
    times.reserve(iterations);
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        mixer.render(output);
        const auto end = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    std::sort(times.begin(), times.end());
    double sum{};
    for (const double value : times) sum += value;
    const auto percentile = [&](double p) {
        const auto index = std::min(times.size() - 1U,
            static_cast<std::size_t>(p * static_cast<double>(times.size() - 1U)));
        return times[index];
    };
    const auto meters = mixer.meters();
    const double deadline = static_cast<double>(blockFrames) * 1000.0 / sampleRate;
    std::cout << std::fixed << std::setprecision(6)
              << "{\n"
              << "  \"sample_rate\": " << sampleRate << ",\n"
              << "  \"block_frames\": " << blockFrames << ",\n"
              << "  \"stream_voices\": " << meters.streamVoices << ",\n"
              << "  \"stream_underrun_frames\": " << meters.streamUnderrunFrames << ",\n"
              << "  \"callback_mean_ms\": " << sum / static_cast<double>(times.size()) << ",\n"
              << "  \"callback_p95_ms\": " << percentile(0.95) << ",\n"
              << "  \"callback_p99_ms\": " << percentile(0.99) << ",\n"
              << "  \"callback_max_ms\": " << times.back() << ",\n"
              << "  \"block_deadline_ms\": " << deadline << ",\n"
              << "  \"p99_deadline_fraction\": " << percentile(0.99) / deadline << "\n"
              << "}\n";
    std::filesystem::remove(path);
    return meters.streamVoices == streamCount && meters.streamUnderrunFrames == 0U ? 0 : 2;
}
