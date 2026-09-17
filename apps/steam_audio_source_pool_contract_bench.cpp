#include "dve/audio/steam_audio_native.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace dve::audio;
    using Clock = std::chrono::steady_clock;
    std::string error;
    SteamAudioNativeSettings native;
    native.frameSize = 256;
    auto context = SteamAudioNativeContext::create(native, &error);
    if (!context) { std::cerr << error << '\n'; return 1; }
    SteamAudioNativeSourcePoolSettings settings;
    settings.maximumSources = 16;
    auto pool = context->create_source_pool(settings, &error);
    if (!pool) { std::cerr << error << '\n'; return 1; }
    AcousticSnapshot scene;
    scene.statistics.sourceGeneration = 1;
    scene.surfaces = std::make_shared<const std::vector<AcousticSurfaceBox>>();
    if (!pool->publish_scene(scene, {}, &error)) { std::cerr << error << '\n'; return 1; }

    AudioListenerState listener;
    listener.forward = {0.0F, 0.0F, 1.0F};
    std::array<SteamAudioNativeSourceRequest, 16> requests{};
    for (std::size_t i = 0; i < requests.size(); ++i) {
        requests[i].sourceId = i + 1U;
        requests[i].emitter.position = {static_cast<float>(i % 4U) - 1.5F, 0.0F,
                                        2.0F + static_cast<float>(i / 4U)};
        requests[i].emitter.forward = {0.0F, 0.0F, -1.0F};
        requests[i].emitter.radiusMeters = 0.1F;
        requests[i].quality = i < 4U ? SteamAudioQuality::Hero : SteamAudioQuality::Important;
        requests[i].perceptualPriority = static_cast<float>(requests.size() - i);
        requests[i].transformGeneration = 1;
    }
    std::array<float, 256> mono{};
    mono[0] = 1.0F;
    std::array<float, 512> stereo{};
    std::vector<double> samples;
    samples.reserve(1000);
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const auto start = Clock::now();
        if (!pool->simulate_batch(listener, requests, 1, &error)) {
            std::cerr << error << '\n'; return 1;
        }
        for (const auto& request : requests) {
            stereo.fill(0.0F);
            const AudioVec3 direction{request.emitter.position.x, request.emitter.position.y,
                                      request.emitter.position.z};
            if (!pool->process_source(request.sourceId, mono, stereo, direction, 1.0F)) return 1;
        }
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    double sum{};
    for (double value : samples) sum += value;
    const auto percentile = [&](double fraction) {
        const std::size_t index = std::min(samples.size() - 1U,
            static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(samples.size()))) - 1U);
        return samples[index];
    };
    const auto metrics = pool->metrics();
    std::cout << "{\n"
              << "  \"kind\": \"deterministic_steam_audio_abi_contract\",\n"
              << "  \"sources\": 16,\n"
              << "  \"frame_size\": 256,\n"
              << "  \"iterations\": 1000,\n"
              << "  \"mean_ms\": " << sum / static_cast<double>(samples.size()) << ",\n"
              << "  \"p95_ms\": " << percentile(0.95) << ",\n"
              << "  \"p99_ms\": " << percentile(0.99) << ",\n"
              << "  \"max_ms\": " << samples.back() << ",\n"
              << "  \"simulation_batches\": " << metrics.simulationBatches << ",\n"
              << "  \"hrtf_process_calls\": " << metrics.hrtfProcessCalls << "\n"
              << "}\n";
}
