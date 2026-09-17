#include "dve/audio/destruction_commit_coordinator.hpp"
#include "dve/audio/synthesizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace {
using namespace dve::audio;

AudioEventAsset collapse_event() {
    AudioEventAsset asset;
    asset.graph.name = "destruction/wall/collapse";
    asset.graph.root = 0;
    asset.graph.nodes.resize(1);
    asset.graph.nodes[0].type = AudioEventNodeType::SynthNote;
    asset.graph.nodes[0].note = 36;
    asset.graph.nodes[0].velocity = 1.0F;
    asset.graph.nodes[0].durationSeconds = 0.3F;
    asset.graph.nodes[0].priority = AudioPriority::Critical;
    return asset;
}

AcousticBuildRequest wall_request(std::uint64_t generation, bool closed) {
    AcousticBuildRequest request;
    request.sourceGeneration = generation;
    request.width = 8;
    request.height = 4;
    request.depth = 4;
    request.cellSizeMeters = 0.5F;
    request.origin = {-2.0F, 0.0F, -1.0F};
    request.resetGrid = closed;
    AcousticBrickUpdate update;
    if (closed) {
        update.width = 8;
        update.height = 4;
        update.depth = 4;
        update.cells.resize(8U * 4U * 4U);
        for (std::uint32_t z = 0; z < 4; ++z)
            for (std::uint32_t y = 0; y < 4; ++y)
                update.cells[4U + 8U * (y + 4U * z)] = {255, 1, 0, 0};
    } else {
        update.originX = 4;
        update.width = 1;
        update.height = 4;
        update.depth = 4;
        update.cells.resize(16U);
    }
    request.dirtyBricks.push_back(std::move(update));
    return request;
}

float interpolate(float a, float b, float amount) noexcept { return a + (b - a) * amount; }

struct OnePole {
    float state{};
    float process(float sample, float cutoff, float sampleRate) noexcept {
        const float coefficient = 1.0F - std::exp(-2.0F * std::numbers::pi_v<float> *
                                                  std::clamp(cutoff, 50.0F, sampleRate * 0.45F) /
                                                  sampleRate);
        state += coefficient * (sample - state);
        return state;
    }
};

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path wavPath = "wall_collapse_acoustic_transition.wav";
    std::filesystem::path jsonPath = "wall_collapse_acoustic_transition.json";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--wav" && index + 1 < argc) wavPath = argv[++index];
        else if (argument == "--json" && index + 1 < argc) jsonPath = argv[++index];
        else {
            std::cerr << "usage: dve_wall_collapse_audio_demo [--wav output.wav] [--json report.json]\n";
            return 2;
        }
    }

    constexpr std::uint32_t sampleRate = 48000;
    AudioMixer mixer(sampleRate);
    AudioEventLibrary events;
    std::string error;
    if (!events.register_asset(collapse_event(), &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    DestructionAudioRuntime runtime(mixer, events);
    runtime.set_default_binding({1, "destruction/wall/collapse", "destruction/wall/collapse",
                                 "destruction/wall/collapse", "destruction/wall/collapse"});
    auto publisher = std::make_shared<AsyncAcousticPublisher>();
    DestructionAudioIngress ingress;
    DestructionCommitCoordinator coordinator;

    if (!coordinator.begin(500, 0.0, &error) ||
        !coordinator.set_acoustic_build(wall_request(500, true)) ||
        !coordinator.commit(ingress, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    (void)ingress.drain(runtime, *publisher, 0.0, true);
    publisher->wait_idle();

    PublishedAcousticSpatializer spatializer(publisher);
    AudioListenerState listener;
    listener.position = {-1.5F, 1.0F, 0.0F};
    listener.forward = {1.0F, 0.0F, 0.0F};
    AudioEmitterState emitter;
    emitter.position = {1.5F, 1.0F, 0.0F};
    const SpatializationResult closed = spatializer.spatialize(listener, emitter);

    if (!coordinator.begin(501, 1.2, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    for (int index = 0; index < 96; ++index) {
        VoxelAudioEdit edit;
        edit.position = {0.25F, 0.1F + static_cast<float>(index % 20) * 0.09F,
                         -0.8F + static_cast<float>(index % 16) * 0.1F};
        edit.velocity = {2.5F, -1.5F, 0.0F};
        edit.material = 1;
        edit.removedVolume = 0.125F;
        edit.fractureArea = 0.25F;
        if (!coordinator.record(edit)) {
            std::cerr << "collapse transaction rejected a voxel edit\n";
            return 1;
        }
    }
    if (!coordinator.set_acoustic_build(wall_request(501, false)) ||
        !coordinator.commit(ingress, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const std::size_t actions = ingress.drain(runtime, *publisher, 2.0, true);
    publisher->wait_idle();
    const SpatializationResult open = spatializer.spatialize(listener, emitter);
    const auto snapshot = publisher->snapshot();

    constexpr float durationSeconds = 3.2F;
    const std::size_t frames = static_cast<std::size_t>(durationSeconds * sampleRate);
    const float collapseSeconds = 1.2F;
    const float crossfadeSeconds = 0.18F;
    std::vector<float> output(frames * 2U);
    OnePole lowPass;
    std::uint32_t noiseState = 0x9e3779b9U;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float time = static_cast<float>(frame) / static_cast<float>(sampleRate);
        const float transition = std::clamp((time - collapseSeconds) / crossfadeSeconds, 0.0F, 1.0F);
        const float gain = interpolate(closed.distanceGain, open.distanceGain, transition);
        const float cutoff = interpolate(closed.lowPassHertz, open.lowPassHertz, transition);
        const float leftGain = interpolate(closed.leftGain, open.leftGain, transition);
        const float rightGain = interpolate(closed.rightGain, open.rightGain, transition);
        float signal = 0.17F * std::sin(2.0F * std::numbers::pi_v<float> * 110.0F * time) +
                       0.08F * std::sin(2.0F * std::numbers::pi_v<float> * 220.0F * time);
        const float relative = time - collapseSeconds;
        if (relative >= 0.0F && relative < 0.55F) {
            noiseState ^= noiseState << 13U;
            noiseState ^= noiseState >> 17U;
            noiseState ^= noiseState << 5U;
            const float noise = static_cast<float>(noiseState & 0xffffU) / 32767.5F - 1.0F;
            const float envelope = std::exp(-relative * 8.0F);
            signal += noise * 0.42F * envelope;
            signal += std::sin(2.0F * std::numbers::pi_v<float> * 48.0F * relative) *
                      0.36F * std::exp(-relative * 5.0F);
        }
        const float filtered = lowPass.process(signal, cutoff, static_cast<float>(sampleRate)) * gain;
        output[frame * 2U] = std::clamp(filtered * leftGain, -1.0F, 1.0F);
        output[frame * 2U + 1U] = std::clamp(filtered * rightGain, -1.0F, 1.0F);
    }

    if (!write_float_wav(wavPath, output, sampleRate, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::ofstream json(jsonPath, std::ios::trunc);
    if (!json) {
        std::cerr << "could not create JSON report\n";
        return 1;
    }
    const auto ingressMetrics = ingress.metrics();
    json << "{\n"
         << "  \"sample_rate\": " << sampleRate << ",\n"
         << "  \"collapse_generation\": 501,\n"
         << "  \"published_acoustic_generation\": "
         << (snapshot ? snapshot->statistics.sourceGeneration : 0) << ",\n"
         << "  \"event_actions\": " << actions << ",\n"
         << "  \"acoustic_requests\": " << ingressMetrics.acousticRequests << ",\n"
         << "  \"closed\": {\"gain\": " << closed.distanceGain
         << ", \"low_pass_hz\": " << closed.lowPassHertz
         << ", \"reverb_send\": " << closed.reverbSend << "},\n"
         << "  \"open\": {\"gain\": " << open.distanceGain
         << ", \"low_pass_hz\": " << open.lowPassHertz
         << ", \"reverb_send\": " << open.reverbSend << "},\n"
         << "  \"transition_seconds\": " << crossfadeSeconds << "\n"
         << "}\n";
    std::cout << "wrote " << wavPath << " and " << jsonPath << '\n';
    return 0;
}
