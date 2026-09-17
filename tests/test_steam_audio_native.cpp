#include "dve/audio/steam_audio_native.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::shared_ptr<const std::vector<dve::audio::AcousticSurfaceBox>> surfaces(
    std::initializer_list<dve::audio::AcousticSurfaceBox> boxes) {
    return std::make_shared<const std::vector<dve::audio::AcousticSurfaceBox>>(boxes);
}
} // namespace

int main() {
    using namespace dve::audio;
    std::string error;
    SteamAudioNativeSettings settings;
    settings.sampleRate = 48000;
    settings.frameSize = 64;
    settings.bilinearInterpolation = true;
    auto context = SteamAudioNativeContext::create(settings, &error);
    require(context && context->valid(), error.c_str());
    require(context->sample_rate() == 48000 && context->frame_size() == 64,
            "native context reported wrong audio settings");

    auto effect = context->create_binaural_effect(&error);
    require(effect && effect->frame_size() == 64 &&
                effect->output_mode() == AudioOutputMode::HeadphonesHrtf,
            error.c_str());
    std::array<float, 64> mono{};
    mono.fill(0.25F);
    std::array<float, 128> stereo{};
    require(effect->process_mono(mono, stereo, {1.0F, 0.0F, 0.0F}, 1.0F),
            "native binaural processing failed");
    float left{}, right{};
    for (std::size_t index = 0; index < 64; ++index) {
        left += std::abs(stereo[index * 2]);
        right += std::abs(stereo[index * 2 + 1]);
    }
    require(right > left * 2.0F, "binaural direction was not passed to the SDK adapter");
    require(!effect->process_mono(std::span<const float>(mono.data(), 32), stereo, {}, 1.0F),
            "native effect accepted the wrong fixed block size");
    effect->reset();

    SteamAudioNativeDirectSettings directSettings;
    directSettings.sourceMatchToleranceMeters = 0.1F;
    auto direct = context->create_direct_simulator(directSettings, &error);
    require(direct && direct->valid(), error.c_str());

    SteamAudioNativeMaterial concrete;
    concrete.absorption = {0.05F, 0.07F, 0.08F};
    concrete.scattering = 0.05F;
    concrete.transmission = {0.015F, 0.002F, 0.001F};
    std::array<SteamAudioNativeMaterial, 2> materials{};
    materials[1] = concrete;

    AcousticSnapshot enclosed;
    enclosed.statistics.sourceGeneration = 10;
    enclosed.surfaces = surfaces({{{-0.15F, 0.0F, -1.0F}, {0.15F, 2.5F, 1.0F}, 1}});
    require(direct->publish_scene(enclosed, materials, &error), error.c_str());

    AudioListenerState listener;
    listener.position = {-2.0F, 1.0F, 0.0F};
    listener.forward = {1.0F, 0.0F, 0.0F};
    AudioEmitterState emitter;
    emitter.position = {2.0F, 1.0F, 0.0F};
    emitter.radiusMeters = 0.2F;
    emitter.reverbSend = 0.25F;
    require(direct->simulate_direct(listener, emitter, SteamAudioQuality::Hero, 10, &error),
            error.c_str());
    const auto blocked = direct->query_direct(listener, emitter, SteamAudioQuality::Hero);
    require(blocked.occlusion > 0.9F, "closed wall was not reported as occluded");
    require(blocked.transmissionMid < 0.01F,
            "closed wall did not use the material transmission coefficients");

    AcousticSnapshot open;
    open.statistics.sourceGeneration = 11;
    open.surfaces = surfaces({});
    require(direct->publish_scene(open, materials, &error), error.c_str());
    require(!direct->publish_scene(enclosed, materials, &error),
            "native scene accepted a stale generation");
    require(direct->simulate_direct(listener, emitter, SteamAudioQuality::Hero, 11, &error),
            error.c_str());
    const auto unblocked = direct->query_direct(listener, emitter, SteamAudioQuality::Hero);
    require(unblocked.occlusion < 0.1F, "removed wall remained acoustically occluded");
    require(unblocked.transmissionMid > 0.99F, "open scene attenuated transmission");
    AudioEmitterState moved = emitter;
    moved.position.x += 1.0F;
    const auto stalePosition = direct->query_direct(listener, moved, SteamAudioQuality::Hero);
    require(!std::isfinite(stalePosition.distanceAttenuation),
            "audio-thread query reused a direct result for a materially moved source");

    const auto metrics = direct->metrics();
    require(metrics.acceptedSceneGenerations == 2 && metrics.rejectedSceneGenerations == 1 &&
                metrics.simulationRuns == 2 && metrics.lastSceneGeneration == 11 &&
                metrics.lastSimulatedGeneration == 11 && metrics.triangles == 0,
            "native direct simulator metrics mismatch");

    SteamAudioNativeSourcePoolSettings poolSettings;
    poolSettings.maximumSources = 8;
    poolSettings.direct.sourceMatchToleranceMeters = 0.15F;
    auto pool = context->create_source_pool(poolSettings, &error);
    require(pool && pool->valid(), error.c_str());
    require(pool->publish_scene(open, materials, &error), error.c_str());
    std::vector<SteamAudioNativeSourceRequest> requests;
    for (std::uint64_t index = 0; index < 12; ++index) {
        SteamAudioNativeSourceRequest request;
        request.sourceId = 100 + index;
        request.emitter = emitter;
        request.emitter.position = {2.0F + static_cast<float>(index) * 0.25F, 1.0F,
                                    static_cast<float>(index % 3U) * 0.5F};
        request.quality = index < 2 ? SteamAudioQuality::Hero :
                          index < 5 ? SteamAudioQuality::Important : SteamAudioQuality::Normal;
        request.perceptualPriority = static_cast<float>(12 - index);
        request.transformGeneration = 11;
        requests.push_back(request);
    }
    require(pool->simulate_batch(listener, requests, 11, &error), error.c_str());
    auto poolMetrics = pool->metrics();
    require(poolMetrics.activeSources == 8 && poolMetrics.capacity == 8 &&
                poolMetrics.simulatedSources == 8 && poolMetrics.rejectedRequests == 0,
            "source pool did not enforce its bounded capacity");
    const auto heroPath = pool->query_direct(listener, requests[0].emitter,
                                              SteamAudioQuality::Hero);
    require(std::isfinite(heroPath.distanceAttenuation),
            "source pool did not publish a selected hero source");
    stereo.fill(0.0F);
    require(pool->process_source(requests[0].sourceId, mono, stereo,
                                 {1.0F, 0.0F, 0.0F}, 1.0F),
            "source pool did not process a preallocated HRTF source");
    require(!pool->process_source(requests[11].sourceId, mono, stereo,
                                  {1.0F, 0.0F, 0.0F}, 1.0F),
            "source pool processed an unselected source");
    const auto droppedPath = pool->query_direct(listener, requests[11].emitter,
                                                 SteamAudioQuality::Normal);
    require(!std::isfinite(droppedPath.distanceAttenuation),
            "source pool published a request below the capacity cutoff");

    std::reverse(requests.begin(), requests.end());
    requests.front().perceptualPriority = 100.0F;
    require(pool->simulate_batch(listener, requests, 11, &error), error.c_str());
    poolMetrics = pool->metrics();
    require(poolMetrics.simulationBatches == 2 && poolMetrics.slotSteals >= 1 &&
                poolMetrics.hrtfProcessCalls == 1 && poolMetrics.hrtfProcessMisses == 1,
            "source pool did not perform deterministic priority stealing/HRTF accounting");

    context.reset(); // effects and simulator retain context lifetime
    require(effect->process_mono(mono, stereo, {-1.0F, 0.0F, 0.0F}, 1.0F),
            "effect did not retain native context lifetime");
    require(direct->valid(), "direct simulator did not retain native context lifetime");
    std::cout << "DVE Steam Audio native scene/HRTF contract tests passed\n";
    return 0;
}
