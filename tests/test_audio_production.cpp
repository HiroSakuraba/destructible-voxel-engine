#include "dve/audio/acoustic_runtime.hpp"
#include "dve/audio/audio_asset.hpp"
#include "dve/audio/destruction_runtime.hpp"
#include "dve/audio/destruction_ingress.hpp"
#include "dve/audio/steam_audio.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

template<class T> void write(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void make_wav(const std::filesystem::path& path) {
    constexpr std::uint32_t sampleRate = 24000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint32_t frames = 2400;
    std::vector<std::int16_t> pcm(frames);
    for (std::uint32_t i = 0; i < frames; ++i)
        pcm[i] = static_cast<std::int16_t>(std::sin(2.0 * 3.141592653589793 * 220.0 * i / sampleRate) * 12000.0);
    const std::uint32_t fmtSize = 16;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    const std::uint32_t riffSize = 4 + 8 + fmtSize + 8 + dataSize;
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4); write(out, riffSize); out.write("WAVE", 4);
    out.write("fmt ", 4); write(out, fmtSize);
    const std::uint16_t format = 1; write(out, format); write(out, channels); write(out, sampleRate);
    const std::uint32_t byteRate = sampleRate * channels * bits / 8; write(out, byteRate);
    const std::uint16_t align = channels * bits / 8; write(out, align); write(out, bits);
    out.write("data", 4); write(out, dataSize);
    out.write(reinterpret_cast<const char*>(pcm.data()), dataSize);
}

void test_assets(const std::filesystem::path& temp) {
    const auto wav = temp / "source.wav";
    const auto cooked = temp / "source.dvesample";
    const auto streamed = temp / "stream.dvesample";
    make_wav(wav);
    dve::audio::AudioImportOptions options;
    options.targetSampleRate = 48000;
    options.normalize = true;
    std::string error;
    auto asset = dve::audio::import_audio_file(wav, options, &error);
    require(asset.has_value(), error.c_str());
    require(asset->metadata.sampleRate == 48000 && asset->metadata.frameCount == 4800,
            "WAV import/resample metadata mismatch");
    require(asset->metadata.peakLinear > 0.94F && asset->metadata.peakLinear <= 0.951F,
            "WAV normalization mismatch");
    require(dve::audio::write_cooked_audio_asset(cooked, *asset, &error), error.c_str());
    auto roundTrip = dve::audio::read_cooked_audio_asset(cooked, &error);
    require(roundTrip && roundTrip->samples == asset->samples, "cooked audio round trip mismatch");
    require(roundTrip->metadata.contentHash == asset->metadata.contentHash, "cooked hash mismatch");

    asset->metadata.storagePolicy = dve::audio::AudioStoragePolicy::Streamed;
    asset->metadata.loop = {100, 1000, true};
    require(dve::audio::write_cooked_audio_asset(streamed, *asset, &error), error.c_str());
    dve::audio::CookedAudioStream stream(streamed, 2048, &error);
    require(stream.valid(), error.c_str());
    for (int i = 0; i < 100 && stream.telemetry().bufferedFrames < 256; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::array<float, 512> output{};
    const std::size_t readFrames = stream.read(output.data(), output.size());
    require(readFrames > 0, "stream worker did not publish frames");
    require(std::any_of(output.begin(), output.end(), [](float value) { return std::abs(value) > 1.0e-4F; }),
            "stream output was silent");

    dve::audio::AudioMixer mixer(48000);
    const dve::audio::StreamSampleId streamId = mixer.register_streamed_sample(streamed, 4096, &error);
    require(static_cast<bool>(streamId), error.c_str());
    dve::audio::PlayStreamDesc play;
    play.sample = streamId;
    play.bus = dve::audio::AudioBusId::Music;
    play.loop = true;
    const auto handle = mixer.play_stream(play);
    require(static_cast<bool>(handle), "mixer rejected streamed playback");
    std::array<float, 512> mixed{};
    bool heard = false;
    for (int attempt = 0; attempt < 100 && !heard; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        mixer.render(mixed);
        heard = std::any_of(mixed.begin(), mixed.end(), [](float value) { return std::abs(value) > 1.0e-4F; });
    }
    require(heard, "mixer streamed playback remained silent");
    require(mixer.meters().streamVoices == 1, "mixer stream voice telemetry mismatch");
    require(mixer.stop(handle, 0.0F), "mixer rejected stream stop");
    mixer.render(mixed);
    require(mixer.meters().streamVoices == 0, "mixer stream voice did not stop");
}

dve::audio::AudioEventAsset make_event(dve::audio::SampleId sample) {
    using namespace dve::audio;
    AudioEventAsset asset;
    asset.graph.name = "destruction/concrete/test";
    asset.deterministicSeed = 1234;
    asset.graph.root = 0;
    asset.graph.nodes.resize(7);
    asset.graph.nodes[0].type = AudioEventNodeType::Layer;
    asset.graph.nodes[0].children = {1, 4};
    asset.graph.nodes[1].type = AudioEventNodeType::Cooldown;
    asset.graph.nodes[1].value = 0.1F;
    asset.graph.nodes[1].children = {2};
    asset.graph.nodes[2].type = AudioEventNodeType::Scatter;
    asset.graph.nodes[2].value = 0.0F;
    asset.graph.nodes[2].value2 = 0.01F;
    asset.graph.nodes[2].count = 3;
    asset.graph.nodes[2].children = {3};
    asset.graph.nodes[3].type = AudioEventNodeType::Sample;
    asset.graph.nodes[3].sample = sample;
    asset.graph.nodes[3].priority = AudioPriority::Important;
    asset.graph.nodes[4].type = AudioEventNodeType::Blend;
    asset.graph.nodes[4].parameter = "removed_volume";
    asset.graph.nodes[4].threshold = 0.0F;
    asset.graph.nodes[4].value2 = 2.0F;
    asset.graph.nodes[4].curve = AudioBlendCurve::EqualPower;
    asset.graph.nodes[4].children = {5, 6};
    asset.graph.nodes[5].type = AudioEventNodeType::Sample;
    asset.graph.nodes[5].sample = sample;
    asset.graph.nodes[6].type = AudioEventNodeType::SynthNote;
    asset.graph.nodes[6].note = 36;
    asset.graph.nodes[6].durationSeconds = 0.05F;
    return asset;
}

void test_event_assets(const std::filesystem::path& temp) {
    using namespace dve::audio;
    auto asset = make_event({1});
    const auto path = temp / "test.dveaudio";
    std::string error;
    require(write_audio_event_asset(path, asset, &error), error.c_str());
    auto loaded = read_audio_event_asset(path, &error);
    require(loaded && loaded->graph.nodes.size() == asset.graph.nodes.size(), "event asset round trip failed");
    auto compiled = compile_audio_event(*loaded, &error);
    require(compiled.has_value(), error.c_str());
    AudioEventInstanceState state;
    AudioEventParameters parameters;
    parameters.values["removed_volume"] = 1.0F;
    auto first = execute_audio_event(*compiled, parameters, state, 48000, 1000);
    require(first.count == 6, "scatter/blend event produced wrong action count");
    auto second = execute_audio_event(*compiled, parameters, state, 48000, 1000);
    require(second.suppressedByCooldown == 1 && second.count == 3,
            "event cooldown did not suppress only its branch");

    AudioEventAsset streamAsset;
    streamAsset.graph.name = "music/stream";
    streamAsset.graph.root = 0;
    streamAsset.graph.nodes.resize(1);
    streamAsset.graph.nodes[0].type = AudioEventNodeType::Stream;
    streamAsset.graph.nodes[0].stream = {1};
    streamAsset.graph.nodes[0].loop = true;
    auto compiledStream = compile_audio_event(streamAsset, &error);
    require(compiledStream.has_value(), error.c_str());
    AudioEventInstanceState streamState;
    const auto streamActions = execute_audio_event(*compiledStream, {}, streamState);
    require(streamActions.count == 1 && streamActions.actions[0].type == AudioEventActionType::PlayStream &&
            streamActions.actions[0].stream.value == 1 && streamActions.actions[0].loop,
            "stream event node did not compile to a looped stream action");
}

void test_destruction_runtime() {
    using namespace dve::audio;
    AudioMixer mixer;
    ResidentSampleDesc sample;
    sample.name = "click";
    sample.channels = 1;
    sample.samples.resize(256);
    for (std::size_t i = 0; i < sample.samples.size(); ++i) sample.samples[i] = (i == 0 ? 0.8F : 0.0F);
    const SampleId sampleId = mixer.register_resident_sample(std::move(sample));
    AudioEventLibrary library;
    std::string error;
    require(library.register_asset(make_event(sampleId), &error), error.c_str());
    DestructionAudioRuntime runtime(mixer, library);
    require(runtime.bind_material({7, "destruction/concrete/test", "destruction/concrete/test",
                                   "destruction/concrete/test", "destruction/concrete/test"}, &error), error.c_str());
    std::vector<VoxelAudioEdit> edits(100);
    for (auto& edit : edits) {
        edit.position = {1.0F, 2.0F, 3.0F};
        edit.material = 7;
        edit.removedVolume = 0.02F;
        edit.fractureArea = 0.1F;
    }
    runtime.submit_voxel_edits(edits, 1.0);
    const auto actions = runtime.update(2.0, true);
    require(actions > 0, "destruction runtime did not dispatch an event");
    std::array<float, 1024> output{};
    mixer.render(output);
    const auto meters = runtime.metrics();
    require(meters.submittedStimuli == 100 && meters.compiledEvents == 1 && meters.dispatchedActions == actions,
            "destruction runtime metrics mismatch");
}


void test_destruction_ingress() {
    using namespace dve::audio;
    AudioMixer mixer;
    ResidentSampleDesc sample;
    sample.name = "ingress-click";
    sample.channels = 1;
    sample.samples.assign(128, 0.0F);
    sample.samples[0] = 0.9F;
    const auto sampleId = mixer.register_resident_sample(std::move(sample));
    AudioEventLibrary library;
    std::string error;
    require(library.register_asset(make_event(sampleId), &error), error.c_str());
    DestructionAudioRuntime runtime(mixer, library);
    require(runtime.bind_material({7, "destruction/concrete/test", "destruction/concrete/test",
                                   "destruction/concrete/test", "destruction/concrete/test"}, &error), error.c_str());
    AsyncAcousticPublisher publisher;
    DestructionAudioIngress ingress;

    DestructionAudioCommit current;
    current.generation = 2;
    current.timeSeconds = 1.0;
    current.voxelEdits.push_back({{1.0F, 1.0F, 1.0F}, {}, 7, 0.5F, 0.25F});
    AcousticBuildRequest acoustic;
    acoustic.width = 2; acoustic.height = 2; acoustic.depth = 2; acoustic.resetGrid = true;
    AcousticBrickUpdate brick;
    brick.width = 2; brick.height = 2; brick.depth = 2; brick.cells.resize(8);
    brick.cells[0] = {255, 1, 0, 0};
    acoustic.dirtyBricks.push_back(std::move(brick));
    current.acousticBuild = std::move(acoustic);
    require(ingress.try_submit(std::move(current)), "current destruction commit was rejected");

    DestructionAudioCommit stale;
    stale.generation = 1;
    stale.timeSeconds = 1.1;
    stale.voxelEdits.push_back({{2.0F, 1.0F, 1.0F}, {}, 7, 0.5F, 0.25F});
    require(ingress.try_submit(std::move(stale)), "stale commit should enter queue before consumer rejects it");
    const auto actions = ingress.drain(runtime, publisher, 2.0, true);
    publisher.wait_idle();
    const auto metrics = ingress.metrics();
    require(metrics.drainedCommits == 1 && metrics.staleCommits == 1 && metrics.acousticRequests == 1,
            "destruction ingress generation accounting mismatch");
    require(metrics.lastDrainedGeneration == 2 && actions > 0,
            "destruction ingress did not dispatch the committed generation");
    const auto snapshot = publisher.snapshot();
    require(snapshot && snapshot->statistics.sourceGeneration == 2,
            "audio/acoustic publication did not preserve the committed generation");

    DestructionAudioIngress bounded;
    for (std::uint64_t i = 1; i <= kDestructionAudioCommitQueueCapacity; ++i) {
        DestructionAudioCommit commit; commit.generation = i; commit.timeSeconds = static_cast<double>(i);
        require(bounded.try_submit(std::move(commit)), "bounded ingress filled prematurely");
    }
    DestructionAudioCommit overflow; overflow.generation = 1000; overflow.timeSeconds = 1000.0;
    require(!bounded.try_submit(std::move(overflow)) && bounded.metrics().rejectedCommits == 1,
            "bounded ingress did not reject overflow without blocking");
    bounded.discard_pending();
    require(bounded.metrics().approximateQueuedCommits == 0,
            "discard_pending did not clear the ingress queue");
}

void test_acoustic_publication() {
    using namespace dve::audio;
    auto publisher = std::make_shared<AsyncAcousticPublisher>();
    AcousticBuildRequest request;
    request.sourceGeneration = 10;
    request.width = 8; request.height = 4; request.depth = 4; request.cellSizeMeters = 0.5F;
    request.resetGrid = true;
    request.rooms = {{1, "A", 20.0F, 0.2F, false}, {2, "B", 20.0F, 0.2F, false}};
    request.portalCandidates.push_back({5, 1, 2, 3, 1, 1, 5, 3, 3, 2.0F, 0.8F});
    AcousticBrickUpdate update;
    update.width = 8; update.height = 4; update.depth = 4;
    update.cells.resize(8 * 4 * 4);
    for (std::uint32_t z = 0; z < 4; ++z)
        for (std::uint32_t y = 0; y < 4; ++y) {
            auto& cell = update.cells[(z * 4 + y) * 8 + 4];
            cell = {255, 3, 0, 0};
        }
    request.dirtyBricks.push_back(update);
    publisher->submit(std::move(request));
    publisher->wait_idle();
    auto first = publisher->snapshot();
    require(first && first->statistics.sourceGeneration == 10 && first->statistics.surfaceBoxes > 0,
            "first acoustic snapshot missing");
    require(first->roomGraph->portals().size() == 1 && first->roomGraph->portals()[0].openness < 1.0F,
            "portal did not observe wall occupancy");

    AcousticBuildRequest hole;
    hole.sourceGeneration = 11;
    hole.width = 8; hole.height = 4; hole.depth = 4; hole.cellSizeMeters = 0.5F;
    AcousticBrickUpdate clear;
    clear.originX = 4; clear.originY = 1; clear.originZ = 1;
    clear.width = 1; clear.height = 2; clear.depth = 2;
    clear.cells.resize(4);
    hole.dirtyBricks.push_back(clear);
    hole.rooms = {{1, "A", 20.0F, 0.2F, false}, {2, "B", 20.0F, 0.2F, false}};
    hole.portalCandidates.push_back({5, 1, 2, 3, 1, 1, 5, 3, 3, 2.0F, 0.8F});
    publisher->submit(std::move(hole));
    publisher->wait_idle();
    auto second = publisher->snapshot();
    require(second && second->statistics.sourceGeneration == 11 &&
            second->roomGraph->portals()[0].openness > first->roomGraph->portals()[0].openness,
            "dynamic portal openness did not increase after hole update");
    PublishedAcousticSpatializer spatializer(publisher);
    const auto result = spatializer.spatialize({}, {{4.0F, 1.0F, 1.0F}});
    require(result.distanceGain >= 0.0F && std::isfinite(result.lowPassHertz),
            "published acoustic spatializer produced invalid output");
}

class FakeSteamBackend final : public dve::audio::ISteamAudioSimulationBackend {
public:
    bool valid() const noexcept override { return true; }
    std::string_view backend_name() const noexcept override { return "contract fake"; }
    dve::audio::SteamAudioDirectPath query_direct(const dve::audio::AudioListenerState&,
                                                   const dve::audio::AudioEmitterState&,
                                                   dve::audio::SteamAudioQuality) const noexcept override {
        dve::audio::SteamAudioDirectPath result;
        result.distanceAttenuation = 0.75F;
        result.occlusion = 0.5F;
        result.transmissionHigh = 0.3F;
        result.reverbSend = 0.4F;
        result.propagationDelaySeconds = 0.02F;
        return result;
    }
};

void test_steam_contract_and_mixer_controls() {
    using namespace dve::audio;
    auto backend = std::make_shared<FakeSteamBackend>();
    SteamAudioSpatializer spatializer(backend, SteamAudioQuality::Hero);
    const auto spatial = spatializer.spatialize({}, {{3.0F, 0.0F, 0.0F}});
    require(spatial.reverbSend >= 0.4F && spatial.lowPassHertz < 20000.0F,
            "Steam Audio contract result was not applied");
    require(spatializer.metrics().backendQueries == 1, "Steam Audio query telemetry mismatch");

    AudioMixer mixer;
    ResidentSampleDesc sample;
    sample.name = "long"; sample.channels = 1; sample.samples.assign(4096, 0.02F);
    const auto id = mixer.register_resident_sample(std::move(sample));
    for (std::size_t i = 0; i < kMaxLogicalSampleVoices + 8; ++i) {
        PlaySampleDesc play; play.sample = id; play.loop = true; play.spatialized = false;
        play.priority = static_cast<AudioPriority>(16U + (i % 200U));
        (void)mixer.play_sample(play);
    }
    AudioBusSnapshot snapshot = mixer.bus_snapshot();
    snapshot.buses[audio_bus_index(AudioBusId::Music)].gain = 0.1F;
    snapshot.buses[audio_bus_index(AudioBusId::Effects)].lowPassHertz = 1000.0F;
    require(mixer.apply_bus_snapshot(snapshot, 0.01F), "mixer snapshot command rejected");
    std::array<float, 2048> output{};
    for (int i = 0; i < 4; ++i) mixer.render(output);
    const auto meters = mixer.meters();
    require(meters.stolenVoices >= 8, "voice-stealing diagnostics did not count replacements");
    require(meters.callbackProfile.renderCalls == 4 && meters.callbackProfile.maximumMilliseconds > 0.0,
            "callback profiler did not record render calls");
}

} // namespace

int main() {
    const auto temp = std::filesystem::temp_directory_path() / "dve_audio_production_tests";
    std::filesystem::remove_all(temp);
    std::filesystem::create_directories(temp);
    test_assets(temp);
    test_event_assets(temp);
    test_destruction_runtime();
    test_destruction_ingress();
    test_acoustic_publication();
    test_steam_contract_and_mixer_controls();
    std::filesystem::remove_all(temp);
    std::cout << "DVE production audio tests passed\n";
    return 0;
}
