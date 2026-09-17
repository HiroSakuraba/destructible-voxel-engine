#include "dve/audio/steam_audio_native.hpp"

#include <phonon.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dve::audio {
namespace {

using Clock = std::chrono::steady_clock;

AudioVec3 normalized(AudioVec3 value, AudioVec3 fallback = {0.0F, 0.0F, -1.0F}) noexcept {
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (!(lengthSquared > 1.0e-12F) || !std::isfinite(lengthSquared)) return fallback;
    const float inverse = 1.0F / std::sqrt(lengthSquared);
    return {value.x * inverse, value.y * inverse, value.z * inverse};
}

AudioVec3 cross(AudioVec3 a, AudioVec3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float distance_squared(AudioVec3 a, AudioVec3 b) noexcept {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return x * x + y * y + z * z;
}

float distance(AudioVec3 a, AudioVec3 b) noexcept {
    return std::sqrt(std::max(0.0F, distance_squared(a, b)));
}

IPLVector3 to_ipl(AudioVec3 value) noexcept { return {value.x, value.y, value.z}; }

IPLCoordinateSpace3 coordinate_space(AudioVec3 position, AudioVec3 forward, AudioVec3 up) noexcept {
    const AudioVec3 ahead = normalized(forward);
    AudioVec3 normalizedUp = normalized(up, {0.0F, 1.0F, 0.0F});
    AudioVec3 right = normalized(cross(ahead, normalizedUp), {1.0F, 0.0F, 0.0F});
    normalizedUp = normalized(cross(right, ahead), {0.0F, 1.0F, 0.0F});
    IPLCoordinateSpace3 result{};
    result.right = to_ipl(right);
    result.up = to_ipl(normalizedUp);
    result.ahead = to_ipl(ahead);
    result.origin = to_ipl(position);
    return result;
}

SteamAudioDirectPath invalid_direct_path() noexcept {
    SteamAudioDirectPath value;
    value.distanceAttenuation = std::numeric_limits<float>::quiet_NaN();
    return value;
}

SteamAudioNativeMaterial sanitized_material(SteamAudioNativeMaterial value) noexcept {
    for (float& coefficient : value.absorption)
        coefficient = std::clamp(std::isfinite(coefficient) ? coefficient : 0.1F, 0.0F, 1.0F);
    for (float& coefficient : value.transmission)
        coefficient = std::clamp(std::isfinite(coefficient) ? coefficient : 0.05F, 0.0F, 1.0F);
    value.scattering = std::clamp(std::isfinite(value.scattering) ? value.scattering : 0.05F,
                                  0.0F, 1.0F);
    return value;
}

IPLMaterial to_ipl(SteamAudioNativeMaterial value) noexcept {
    value = sanitized_material(value);
    IPLMaterial material{};
    for (std::size_t band = 0; band < 3U; ++band) {
        material.absorption[band] = value.absorption[band];
        material.transmission[band] = value.transmission[band];
    }
    material.scattering = value.scattering;
    return material;
}

void append_box(const AcousticSurfaceBox& box, std::int32_t materialIndex,
                std::vector<IPLVector3>& vertices, std::vector<IPLTriangle>& triangles,
                std::vector<IPLint32>& materialIndices) {
    const IPLint32 base = static_cast<IPLint32>(vertices.size());
    const AudioVec3 minimum = box.minimum;
    const AudioVec3 maximum = box.maximum;
    vertices.push_back({minimum.x, minimum.y, minimum.z});
    vertices.push_back({maximum.x, minimum.y, minimum.z});
    vertices.push_back({maximum.x, maximum.y, minimum.z});
    vertices.push_back({minimum.x, maximum.y, minimum.z});
    vertices.push_back({minimum.x, minimum.y, maximum.z});
    vertices.push_back({maximum.x, minimum.y, maximum.z});
    vertices.push_back({maximum.x, maximum.y, maximum.z});
    vertices.push_back({minimum.x, maximum.y, maximum.z});

    constexpr std::array<std::array<IPLint32, 3>, 12> localTriangles{{
        {{0, 2, 1}}, {{0, 3, 2}}, // -Z
        {{4, 5, 6}}, {{4, 6, 7}}, // +Z
        {{0, 4, 7}}, {{0, 7, 3}}, // -X
        {{1, 2, 6}}, {{1, 6, 5}}, // +X
        {{0, 1, 5}}, {{0, 5, 4}}, // -Y
        {{3, 7, 6}}, {{3, 6, 2}}  // +Y
    }};
    for (const auto& local : localTriangles) {
        IPLTriangle triangle{};
        triangle.indices[0] = base + local[0];
        triangle.indices[1] = base + local[1];
        triangle.indices[2] = base + local[2];
        triangles.push_back(triangle);
        materialIndices.push_back(materialIndex);
    }
}

bool valid_box(const AcousticSurfaceBox& box) noexcept {
    return std::isfinite(box.minimum.x) && std::isfinite(box.minimum.y) &&
           std::isfinite(box.minimum.z) && std::isfinite(box.maximum.x) &&
           std::isfinite(box.maximum.y) && std::isfinite(box.maximum.z) &&
           box.maximum.x > box.minimum.x && box.maximum.y > box.minimum.y &&
           box.maximum.z > box.minimum.z;
}

class SteamAudioNativeBinauralEffect final : public ISpatialAudioBlockEffect {
public:
    SteamAudioNativeBinauralEffect(std::shared_ptr<SteamAudioNativeContext> owner,
                                   IPLHRTF hrtf, IPLBinauralEffect effect,
                                   std::size_t frameSize, bool bilinear) noexcept
        : owner_(std::move(owner)), hrtf_(hrtf), effect_(effect), frameSize_(frameSize),
          bilinear_(bilinear), left_(frameSize), right_(frameSize) {}
    ~SteamAudioNativeBinauralEffect() override {
        if (effect_) iplBinauralEffectRelease(&effect_);
    }

    [[nodiscard]] std::size_t frame_size() const noexcept override { return frameSize_; }
    [[nodiscard]] AudioOutputMode output_mode() const noexcept override {
        return AudioOutputMode::HeadphonesHrtf;
    }

    bool process_mono(std::span<const float> monoInput,
                      std::span<float> interleavedOutput,
                      AudioVec3 listenerRelativeDirection,
                      float spatialBlend) noexcept override {
        if (!effect_ || monoInput.size() != frameSize_ ||
            interleavedOutput.size() != frameSize_ * 2U) return false;
        const AudioVec3 direction = normalized(listenerRelativeDirection);
        float* inputChannels[1]{const_cast<float*>(monoInput.data())};
        float* outputChannels[2]{left_.data(), right_.data()};
        IPLAudioBuffer input{};
        input.numChannels = 1;
        input.numSamples = static_cast<IPLint32>(frameSize_);
        input.data = inputChannels;
        IPLAudioBuffer output{};
        output.numChannels = 2;
        output.numSamples = static_cast<IPLint32>(frameSize_);
        output.data = outputChannels;
        IPLBinauralEffectParams params{};
        params.direction = to_ipl(direction);
        params.interpolation = bilinear_ ? IPL_HRTFINTERPOLATION_BILINEAR
                                         : IPL_HRTFINTERPOLATION_NEAREST;
        params.spatialBlend = std::clamp(spatialBlend, 0.0F, 1.0F);
        params.hrtf = hrtf_;
        params.peakDelays = nullptr;
        (void)iplBinauralEffectApply(effect_, &params, &input, &output);
        for (std::size_t frame = 0; frame < frameSize_; ++frame) {
            interleavedOutput[frame * 2U] = left_[frame];
            interleavedOutput[frame * 2U + 1U] = right_[frame];
        }
        return true;
    }

    void reset() noexcept override {
        if (effect_) iplBinauralEffectReset(effect_);
        std::fill(left_.begin(), left_.end(), 0.0F);
        std::fill(right_.begin(), right_.end(), 0.0F);
    }

private:
    std::shared_ptr<SteamAudioNativeContext> owner_;
    IPLHRTF hrtf_{};
    IPLBinauralEffect effect_{};
    std::size_t frameSize_{};
    bool bilinear_{};
    std::vector<float> left_;
    std::vector<float> right_;
};

struct PublishedDirectState {
    AudioListenerState listener{};
    AudioEmitterState emitter{};
    SteamAudioQuality quality{SteamAudioQuality::Normal};
    std::uint64_t sourceGeneration{};
    SteamAudioDirectPath path{};
};

} // namespace

struct SteamAudioNativeContext::Impl {
    IPLContext context{};
    IPLHRTF hrtf{};
    IPLAudioSettings audio{};
    std::size_t frameSize{};
    bool bilinear{};
    std::string sofaFile;

    ~Impl() {
        if (hrtf) iplHRTFRelease(&hrtf);
        if (context) iplContextRelease(&context);
    }
};

SteamAudioNativeContext::SteamAudioNativeContext(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SteamAudioNativeContext::~SteamAudioNativeContext() = default;

std::shared_ptr<SteamAudioNativeContext> SteamAudioNativeContext::create(
    const SteamAudioNativeSettings& settings, std::string* error) {
    if (settings.sampleRate < 8000U || settings.sampleRate > 384000U ||
        settings.frameSize == 0U || settings.frameSize > 8192U) {
        if (error) *error = "invalid Steam Audio sample rate or frame size";
        return {};
    }
    auto impl = std::make_unique<Impl>();
    impl->frameSize = settings.frameSize;
    impl->bilinear = settings.bilinearInterpolation;
    impl->sofaFile = settings.sofaFile;
    impl->audio.samplingRate = static_cast<IPLint32>(settings.sampleRate);
    impl->audio.frameSize = static_cast<IPLint32>(settings.frameSize);

    IPLContextSettings contextSettings{};
    contextSettings.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&contextSettings, &impl->context) != IPL_STATUS_SUCCESS || !impl->context) {
        if (error) *error = "iplContextCreate failed";
        return {};
    }
    IPLHRTFSettings hrtfSettings{};
    hrtfSettings.type = impl->sofaFile.empty() ? IPL_HRTFTYPE_DEFAULT : IPL_HRTFTYPE_SOFA;
    hrtfSettings.sofaFileName = impl->sofaFile.empty() ? nullptr : impl->sofaFile.c_str();
    if (iplHRTFCreate(impl->context, &impl->audio, &hrtfSettings, &impl->hrtf) !=
            IPL_STATUS_SUCCESS ||
        !impl->hrtf) {
        if (error) *error = "iplHRTFCreate failed";
        return {};
    }
    return std::shared_ptr<SteamAudioNativeContext>(new SteamAudioNativeContext(std::move(impl)));
}

bool SteamAudioNativeContext::valid() const noexcept {
    return impl_ && impl_->context && impl_->hrtf;
}
std::uint32_t SteamAudioNativeContext::sample_rate() const noexcept {
    return valid() ? static_cast<std::uint32_t>(impl_->audio.samplingRate) : 0U;
}
std::size_t SteamAudioNativeContext::frame_size() const noexcept {
    return valid() ? impl_->frameSize : 0U;
}

std::unique_ptr<ISpatialAudioBlockEffect> SteamAudioNativeContext::create_binaural_effect(
    std::string* error) {
    if (!valid()) {
        if (error) *error = "Steam Audio context is not valid";
        return {};
    }
    IPLBinauralEffectSettings settings{};
    settings.hrtf = impl_->hrtf;
    IPLBinauralEffect effect{};
    if (iplBinauralEffectCreate(impl_->context, &impl_->audio, &settings, &effect) !=
            IPL_STATUS_SUCCESS ||
        !effect) {
        if (error) *error = "iplBinauralEffectCreate failed";
        return {};
    }
    return std::make_unique<SteamAudioNativeBinauralEffect>(shared_from_this(), impl_->hrtf,
                                                            effect, impl_->frameSize,
                                                            impl_->bilinear);
}

struct SteamAudioNativeDirectSimulator::Impl {
    std::shared_ptr<SteamAudioNativeContext> owner;
    SteamAudioNativeDirectSettings settings{};
    IPLScene scene{};
    IPLStaticMesh mesh{};
    IPLSimulator simulator{};
    IPLSource source{};
    std::atomic<std::shared_ptr<const PublishedDirectState>> published{};
    std::atomic<std::uint64_t> sceneGeneration{};
    std::atomic<std::uint64_t> acceptedSceneGenerations{};
    std::atomic<std::uint64_t> rejectedSceneGenerations{};
    std::atomic<std::uint64_t> simulationRuns{};
    mutable std::atomic<std::uint64_t> queryHits{};
    mutable std::atomic<std::uint64_t> queryMisses{};
    std::atomic<std::uint64_t> lastSimulatedGeneration{};
    std::atomic<std::uint64_t> vertices{};
    std::atomic<std::uint64_t> triangles{};
    std::atomic<double> lastSceneBuildMilliseconds{};
    std::atomic<double> lastSimulationMilliseconds{};

    ~Impl() {
        if (source && simulator) {
            iplSourceRemove(source, simulator);
            iplSimulatorCommit(simulator);
        }
        if (mesh && scene) {
            iplStaticMeshRemove(mesh, scene);
            iplSceneCommit(scene);
        }
        if (source) iplSourceRelease(&source);
        if (simulator) iplSimulatorRelease(&simulator);
        if (mesh) iplStaticMeshRelease(&mesh);
        if (scene) iplSceneRelease(&scene);
    }
};

SteamAudioNativeDirectSimulator::SteamAudioNativeDirectSimulator(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SteamAudioNativeDirectSimulator::~SteamAudioNativeDirectSimulator() = default;

std::shared_ptr<SteamAudioNativeDirectSimulator> SteamAudioNativeContext::create_direct_simulator(
    const SteamAudioNativeDirectSettings& requestedSettings, std::string* error) {
    if (!valid()) {
        if (error) *error = "Steam Audio context is not valid";
        return {};
    }
    SteamAudioNativeDirectSettings settings = requestedSettings;
    settings.maximumOcclusionSamples = std::clamp(settings.maximumOcclusionSamples, 1, 1024);
    settings.normalOcclusionSamples = std::clamp(settings.normalOcclusionSamples, 1,
                                                  settings.maximumOcclusionSamples);
    settings.importantOcclusionSamples = std::clamp(settings.importantOcclusionSamples, 1,
                                                     settings.maximumOcclusionSamples);
    settings.heroOcclusionSamples = std::clamp(settings.heroOcclusionSamples, 1,
                                                settings.maximumOcclusionSamples);
    settings.transmissionRays = std::clamp(settings.transmissionRays, 1, 64);
    if (!(settings.sourceMatchToleranceMeters > 0.0F) ||
        !std::isfinite(settings.sourceMatchToleranceMeters))
        settings.sourceMatchToleranceMeters = 0.25F;

    auto impl = std::make_unique<SteamAudioNativeDirectSimulator::Impl>();
    impl->owner = shared_from_this();
    impl->settings = settings;

    IPLSceneSettings sceneSettings{};
    sceneSettings.type = IPL_SCENETYPE_DEFAULT;
    if (iplSceneCreate(impl_->context, &sceneSettings, &impl->scene) != IPL_STATUS_SUCCESS ||
        !impl->scene) {
        if (error) *error = "iplSceneCreate failed";
        return {};
    }

    IPLSimulationSettings simulationSettings{};
    simulationSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    simulationSettings.sceneType = IPL_SCENETYPE_DEFAULT;
    simulationSettings.reflectionType = static_cast<IPLReflectionEffectType>(0);
    simulationSettings.maxNumOcclusionSamples = settings.maximumOcclusionSamples;
    simulationSettings.maxNumSources = 1;
    simulationSettings.numThreads = 1;
    simulationSettings.samplingRate = impl_->audio.samplingRate;
    simulationSettings.frameSize = impl_->audio.frameSize;
    if (iplSimulatorCreate(impl_->context, &simulationSettings, &impl->simulator) !=
            IPL_STATUS_SUCCESS ||
        !impl->simulator) {
        if (error) *error = "iplSimulatorCreate failed";
        return {};
    }
    iplSimulatorSetScene(impl->simulator, impl->scene);

    IPLSourceSettings sourceSettings{};
    sourceSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    if (iplSourceCreate(impl->simulator, &sourceSettings, &impl->source) != IPL_STATUS_SUCCESS ||
        !impl->source) {
        if (error) *error = "iplSourceCreate failed";
        return {};
    }
    iplSourceAdd(impl->source, impl->simulator);
    iplSimulatorCommit(impl->simulator);
    return std::shared_ptr<SteamAudioNativeDirectSimulator>(
        new SteamAudioNativeDirectSimulator(std::move(impl)));
}

bool SteamAudioNativeDirectSimulator::valid() const noexcept {
    return impl_ && impl_->owner && impl_->owner->valid() && impl_->scene && impl_->simulator &&
           impl_->source;
}

std::string_view SteamAudioNativeDirectSimulator::backend_name() const noexcept {
    return "Steam Audio native direct simulation";
}

bool SteamAudioNativeDirectSimulator::publish_scene(
    const AcousticSnapshot& snapshot, std::span<const SteamAudioNativeMaterial> sourceMaterials,
    std::string* error) {
    if (!valid()) {
        if (error) *error = "Steam Audio direct simulator is not valid";
        return false;
    }
    const std::uint64_t generation = snapshot.statistics.sourceGeneration;
    if (generation == 0U || generation <= impl_->sceneGeneration.load(std::memory_order_acquire)) {
        impl_->rejectedSceneGenerations.fetch_add(1U, std::memory_order_relaxed);
        if (error) *error = "Steam Audio scene generation is zero or stale";
        return false;
    }
    const auto started = Clock::now();
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> materialIndices;
    std::uint8_t maximumMaterial{};
    const auto* surfaces = snapshot.surfaces.get();
    if (surfaces) {
        vertices.reserve(surfaces->size() * 8U);
        triangles.reserve(surfaces->size() * 12U);
        materialIndices.reserve(surfaces->size() * 12U);
        for (const auto& box : *surfaces) {
            if (!valid_box(box)) continue;
            maximumMaterial = std::max(maximumMaterial, box.material);
            append_box(box, static_cast<std::int32_t>(box.material), vertices, triangles,
                       materialIndices);
        }
    }
    const std::size_t materialCount = std::max<std::size_t>(1U,
        std::max<std::size_t>(static_cast<std::size_t>(maximumMaterial) + 1U,
                              sourceMaterials.size()));
    std::vector<IPLMaterial> materials(materialCount);
    for (std::size_t index = 0; index < materialCount; ++index) {
        const SteamAudioNativeMaterial value = index < sourceMaterials.size()
                                                   ? sourceMaterials[index]
                                                   : SteamAudioNativeMaterial{};
        materials[index] = to_ipl(value);
    }

    IPLStaticMesh replacement{};
    if (!triangles.empty()) {
        IPLStaticMeshSettings meshSettings{};
        meshSettings.numVertices = static_cast<IPLint32>(vertices.size());
        meshSettings.numTriangles = static_cast<IPLint32>(triangles.size());
        meshSettings.numMaterials = static_cast<IPLint32>(materials.size());
        meshSettings.vertices = vertices.data();
        meshSettings.triangles = triangles.data();
        meshSettings.materialIndices = materialIndices.data();
        meshSettings.materials = materials.data();
        if (iplStaticMeshCreate(impl_->scene, &meshSettings, &replacement) != IPL_STATUS_SUCCESS ||
            !replacement) {
            if (error) *error = "iplStaticMeshCreate failed";
            return false;
        }
        iplStaticMeshAdd(replacement, impl_->scene);
    }
    IPLStaticMesh previous = impl_->mesh;
    if (previous) iplStaticMeshRemove(previous, impl_->scene);
    iplSceneCommit(impl_->scene);
    iplSimulatorSetScene(impl_->simulator, impl_->scene);
    iplSimulatorCommit(impl_->simulator);
    impl_->mesh = replacement;
    if (previous) iplStaticMeshRelease(&previous);

    impl_->sceneGeneration.store(generation, std::memory_order_release);
    impl_->acceptedSceneGenerations.fetch_add(1U, std::memory_order_relaxed);
    impl_->vertices.store(vertices.size(), std::memory_order_relaxed);
    impl_->triangles.store(triangles.size(), std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    impl_->lastSceneBuildMilliseconds.store(elapsed, std::memory_order_relaxed);
    return true;
}

bool SteamAudioNativeDirectSimulator::simulate_direct(
    const AudioListenerState& listener, const AudioEmitterState& emitter,
    SteamAudioQuality quality, std::uint64_t sourceGeneration, std::string* error) {
    if (!valid()) {
        if (error) *error = "Steam Audio direct simulator is not valid";
        return false;
    }
    const std::uint64_t sceneGeneration = impl_->sceneGeneration.load(std::memory_order_acquire);
    if (sourceGeneration == 0U || sourceGeneration != sceneGeneration) {
        if (error) *error = "direct simulation generation does not match the committed scene";
        return false;
    }
    const auto started = Clock::now();
    IPLSimulationInputs inputs{};
    inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
    inputs.directFlags = static_cast<IPLDirectSimulationFlags>(
        IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION |
        IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION |
        IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY |
        IPL_DIRECTSIMULATIONFLAGS_OCCLUSION |
        IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
    inputs.source = coordinate_space(emitter.position, emitter.forward, {0.0F, 1.0F, 0.0F});
    inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
    inputs.distanceAttenuationModel.minDistance = std::max(0.01F, emitter.minDistanceMeters);
    inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    inputs.directivity.dipoleWeight = std::clamp(emitter.directivity, 0.0F, 1.0F);
    inputs.directivity.dipolePower = 1.0F;
    inputs.occlusionRadius = std::max(0.01F, emitter.radiusMeters);
    switch (quality) {
        case SteamAudioQuality::Hero:
            inputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
            inputs.numOcclusionSamples = impl_->settings.heroOcclusionSamples;
            break;
        case SteamAudioQuality::Important:
            inputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
            inputs.numOcclusionSamples = impl_->settings.importantOcclusionSamples;
            break;
        case SteamAudioQuality::Normal:
            inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
            inputs.numOcclusionSamples = impl_->settings.normalOcclusionSamples;
            break;
        case SteamAudioQuality::Background:
            inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
            inputs.numOcclusionSamples = 1;
            break;
    }
    inputs.reverbScale[0] = 1.0F;
    inputs.reverbScale[1] = 1.0F;
    inputs.reverbScale[2] = 1.0F;
    inputs.numTransmissionRays = impl_->settings.transmissionRays;

    IPLSimulationSharedInputs shared{};
    shared.listener = coordinate_space(listener.position, listener.forward, listener.up);
    iplSourceSetInputs(impl_->source, IPL_SIMULATIONFLAGS_DIRECT, &inputs);
    iplSimulatorSetSharedInputs(impl_->simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);
    iplSimulatorRunDirect(impl_->simulator);
    IPLSimulationOutputs outputs{};
    iplSourceGetOutputs(impl_->source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);

    SteamAudioDirectPath path;
    path.distanceAttenuation = outputs.direct.distanceAttenuation;
    path.airAbsorptionLow = outputs.direct.airAbsorption[0];
    path.airAbsorptionMid = outputs.direct.airAbsorption[1];
    path.airAbsorptionHigh = outputs.direct.airAbsorption[2];
    path.directivity = outputs.direct.directivity;
    // Steam Audio reports visibility: 1 is open and 0 is fully occluded. DVE's portable
    // contract stores obstruction: 0 is open and 1 is fully occluded.
    path.occlusion = 1.0F - std::clamp(outputs.direct.occlusion, 0.0F, 1.0F);
    path.transmissionLow = outputs.direct.transmission[0];
    path.transmissionMid = outputs.direct.transmission[1];
    path.transmissionHigh = outputs.direct.transmission[2];
    path.reverbSend = std::clamp(emitter.reverbSend, 0.0F, 1.0F);
    path.propagationDelaySeconds = distance(listener.position, emitter.position) / 343.0F;

    auto state = std::make_shared<PublishedDirectState>();
    state->listener = listener;
    state->emitter = emitter;
    state->quality = quality;
    state->sourceGeneration = sourceGeneration;
    state->path = path;
    impl_->published.store(std::move(state), std::memory_order_release);
    impl_->lastSimulatedGeneration.store(sourceGeneration, std::memory_order_release);
    impl_->simulationRuns.fetch_add(1U, std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    impl_->lastSimulationMilliseconds.store(elapsed, std::memory_order_relaxed);
    return true;
}

SteamAudioDirectPath SteamAudioNativeDirectSimulator::query_direct(
    const AudioListenerState& listener, const AudioEmitterState& emitter,
    SteamAudioQuality quality) const noexcept {
    if (!valid()) return invalid_direct_path();
    const auto state = impl_->published.load(std::memory_order_acquire);
    const float toleranceSquared = impl_->settings.sourceMatchToleranceMeters *
                                   impl_->settings.sourceMatchToleranceMeters;
    if (!state || state->quality != quality ||
        state->sourceGeneration != impl_->sceneGeneration.load(std::memory_order_acquire) ||
        distance_squared(state->listener.position, listener.position) > toleranceSquared ||
        distance_squared(state->emitter.position, emitter.position) > toleranceSquared) {
        impl_->queryMisses.fetch_add(1U, std::memory_order_relaxed);
        return invalid_direct_path();
    }
    impl_->queryHits.fetch_add(1U, std::memory_order_relaxed);
    return state->path;
}

SteamAudioNativeDirectMetrics SteamAudioNativeDirectSimulator::metrics() const noexcept {
    if (!impl_) return {};
    return {impl_->acceptedSceneGenerations.load(std::memory_order_relaxed),
            impl_->rejectedSceneGenerations.load(std::memory_order_relaxed),
            impl_->simulationRuns.load(std::memory_order_relaxed),
            impl_->queryHits.load(std::memory_order_relaxed),
            impl_->queryMisses.load(std::memory_order_relaxed),
            impl_->sceneGeneration.load(std::memory_order_acquire),
            impl_->lastSimulatedGeneration.load(std::memory_order_acquire),
            impl_->vertices.load(std::memory_order_relaxed),
            impl_->triangles.load(std::memory_order_relaxed),
            impl_->lastSceneBuildMilliseconds.load(std::memory_order_relaxed),
            impl_->lastSimulationMilliseconds.load(std::memory_order_relaxed)};
}


namespace {

int quality_rank(SteamAudioQuality quality) noexcept {
    switch (quality) {
        case SteamAudioQuality::Hero: return 3;
        case SteamAudioQuality::Important: return 2;
        case SteamAudioQuality::Normal: return 1;
        case SteamAudioQuality::Background: return 0;
    }
    return 0;
}

struct PublishedPoolEntry {
    std::uint64_t sourceId{};
    AudioListenerState listener{};
    AudioEmitterState emitter{};
    SteamAudioQuality quality{SteamAudioQuality::Normal};
    float perceptualPriority{};
    std::uint64_t transformGeneration{};
    std::size_t slot{};
    SteamAudioDirectPath path{};
};

struct PublishedPoolState {
    std::uint64_t sceneGeneration{};
    std::vector<PublishedPoolEntry> entries;
};

bool finite_emitter(const AudioEmitterState& e) noexcept {
    return std::isfinite(e.position.x) && std::isfinite(e.position.y) &&
           std::isfinite(e.position.z) && std::isfinite(e.velocity.x) &&
           std::isfinite(e.velocity.y) && std::isfinite(e.velocity.z) &&
           std::isfinite(e.forward.x) && std::isfinite(e.forward.y) &&
           std::isfinite(e.forward.z) && std::isfinite(e.radiusMeters) &&
           std::isfinite(e.minDistanceMeters) && std::isfinite(e.maxDistanceMeters) &&
           std::isfinite(e.directivity) && std::isfinite(e.reverbSend);
}

IPLSimulationInputs direct_inputs(const AudioEmitterState& emitter, SteamAudioQuality quality,
                                  const SteamAudioNativeDirectSettings& settings) noexcept {
    IPLSimulationInputs inputs{};
    inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
    inputs.directFlags = static_cast<IPLDirectSimulationFlags>(
        IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION |
        IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION |
        IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY |
        IPL_DIRECTSIMULATIONFLAGS_OCCLUSION |
        IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
    inputs.source = coordinate_space(emitter.position, emitter.forward, {0.0F, 1.0F, 0.0F});
    inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
    inputs.distanceAttenuationModel.minDistance = std::max(0.01F, emitter.minDistanceMeters);
    inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    inputs.directivity.dipoleWeight = std::clamp(emitter.directivity, 0.0F, 1.0F);
    inputs.directivity.dipolePower = 1.0F;
    inputs.occlusionRadius = std::max(0.01F, emitter.radiusMeters);
    switch (quality) {
        case SteamAudioQuality::Hero:
            inputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
            inputs.numOcclusionSamples = settings.heroOcclusionSamples;
            break;
        case SteamAudioQuality::Important:
            inputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
            inputs.numOcclusionSamples = settings.importantOcclusionSamples;
            break;
        case SteamAudioQuality::Normal:
            inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
            inputs.numOcclusionSamples = settings.normalOcclusionSamples;
            break;
        case SteamAudioQuality::Background:
            inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
            inputs.numOcclusionSamples = 1;
            break;
    }
    inputs.reverbScale[0] = inputs.reverbScale[1] = inputs.reverbScale[2] = 1.0F;
    inputs.numTransmissionRays = settings.transmissionRays;
    return inputs;
}

SteamAudioDirectPath direct_path_from_outputs(const IPLSimulationOutputs& outputs,
                                              const AudioListenerState& listener,
                                              const AudioEmitterState& emitter) noexcept {
    SteamAudioDirectPath path;
    path.distanceAttenuation = outputs.direct.distanceAttenuation;
    path.airAbsorptionLow = outputs.direct.airAbsorption[0];
    path.airAbsorptionMid = outputs.direct.airAbsorption[1];
    path.airAbsorptionHigh = outputs.direct.airAbsorption[2];
    path.directivity = outputs.direct.directivity;
    path.occlusion = 1.0F - std::clamp(outputs.direct.occlusion, 0.0F, 1.0F);
    path.transmissionLow = outputs.direct.transmission[0];
    path.transmissionMid = outputs.direct.transmission[1];
    path.transmissionHigh = outputs.direct.transmission[2];
    path.reverbSend = std::clamp(emitter.reverbSend, 0.0F, 1.0F);
    path.propagationDelaySeconds = distance(listener.position, emitter.position) / 343.0F;
    return path;
}

} // namespace

struct SteamAudioNativeSourcePool::Impl {
    struct Slot {
        IPLSource source{};
        std::unique_ptr<ISpatialAudioBlockEffect> binaural;
        std::uint64_t sourceId{};
        float priority{};
        SteamAudioQuality quality{SteamAudioQuality::Background};
    };

    std::shared_ptr<SteamAudioNativeContext> owner;
    SteamAudioNativeSourcePoolSettings settings{};
    IPLScene scene{};
    IPLStaticMesh mesh{};
    IPLSimulator simulator{};
    std::vector<Slot> slots;
    std::atomic<std::shared_ptr<const PublishedPoolState>> published{};
    std::atomic<std::uint64_t> sceneGeneration{};
    std::atomic<std::uint64_t> acceptedSceneGenerations{};
    std::atomic<std::uint64_t> rejectedSceneGenerations{};
    std::atomic<std::uint64_t> simulationBatches{};
    std::atomic<std::uint64_t> simulatedSources{};
    std::atomic<std::uint64_t> rejectedRequests{};
    std::atomic<std::uint64_t> slotAssignments{};
    std::atomic<std::uint64_t> slotSteals{};
    mutable std::atomic<std::uint64_t> queryHits{};
    mutable std::atomic<std::uint64_t> queryMisses{};
    std::atomic<std::uint64_t> hrtfProcessCalls{};
    std::atomic<std::uint64_t> hrtfProcessMisses{};
    std::atomic<std::size_t> activeSources{};
    std::atomic<double> lastBatchMilliseconds{};

    ~Impl() {
        if (simulator) {
            for (auto& slot : slots) if (slot.source) iplSourceRemove(slot.source, simulator);
            iplSimulatorCommit(simulator);
        }
        if (mesh && scene) {
            iplStaticMeshRemove(mesh, scene);
            iplSceneCommit(scene);
        }
        for (auto& slot : slots) if (slot.source) iplSourceRelease(&slot.source);
        if (simulator) iplSimulatorRelease(&simulator);
        if (mesh) iplStaticMeshRelease(&mesh);
        if (scene) iplSceneRelease(&scene);
    }
};

SteamAudioNativeSourcePool::SteamAudioNativeSourcePool(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SteamAudioNativeSourcePool::~SteamAudioNativeSourcePool() = default;

std::shared_ptr<SteamAudioNativeSourcePool> SteamAudioNativeContext::create_source_pool(
    const SteamAudioNativeSourcePoolSettings& requested, std::string* error) {
    if (!valid()) {
        if (error) *error = "Steam Audio context is not valid";
        return {};
    }
    SteamAudioNativeSourcePoolSettings settings = requested;
    settings.maximumSources = std::clamp<std::size_t>(settings.maximumSources, 1U, 16U);
    settings.direct.maximumOcclusionSamples =
        std::clamp(settings.direct.maximumOcclusionSamples, 1, 1024);
    settings.direct.normalOcclusionSamples = std::clamp(settings.direct.normalOcclusionSamples, 1,
        settings.direct.maximumOcclusionSamples);
    settings.direct.importantOcclusionSamples = std::clamp(settings.direct.importantOcclusionSamples, 1,
        settings.direct.maximumOcclusionSamples);
    settings.direct.heroOcclusionSamples = std::clamp(settings.direct.heroOcclusionSamples, 1,
        settings.direct.maximumOcclusionSamples);
    settings.direct.transmissionRays = std::clamp(settings.direct.transmissionRays, 1, 64);
    if (!(settings.direct.sourceMatchToleranceMeters > 0.0F) ||
        !std::isfinite(settings.direct.sourceMatchToleranceMeters))
        settings.direct.sourceMatchToleranceMeters = 0.25F;

    auto impl = std::make_unique<SteamAudioNativeSourcePool::Impl>();
    impl->owner = shared_from_this();
    impl->settings = settings;
    IPLSceneSettings sceneSettings{};
    sceneSettings.type = IPL_SCENETYPE_DEFAULT;
    if (iplSceneCreate(impl_->context, &sceneSettings, &impl->scene) != IPL_STATUS_SUCCESS ||
        !impl->scene) {
        if (error) *error = "iplSceneCreate failed for source pool";
        return {};
    }
    IPLSimulationSettings simulationSettings{};
    simulationSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    simulationSettings.sceneType = IPL_SCENETYPE_DEFAULT;
    simulationSettings.reflectionType = static_cast<IPLReflectionEffectType>(0);
    simulationSettings.maxNumOcclusionSamples = settings.direct.maximumOcclusionSamples;
    simulationSettings.maxNumSources = static_cast<IPLint32>(settings.maximumSources);
    simulationSettings.numThreads = 1;
    simulationSettings.samplingRate = impl_->audio.samplingRate;
    simulationSettings.frameSize = impl_->audio.frameSize;
    if (iplSimulatorCreate(impl_->context, &simulationSettings, &impl->simulator) !=
            IPL_STATUS_SUCCESS || !impl->simulator) {
        if (error) *error = "iplSimulatorCreate failed for source pool";
        return {};
    }
    iplSimulatorSetScene(impl->simulator, impl->scene);
    impl->slots.resize(settings.maximumSources);
    IPLSourceSettings sourceSettings{};
    sourceSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    for (auto& slot : impl->slots) {
        if (iplSourceCreate(impl->simulator, &sourceSettings, &slot.source) != IPL_STATUS_SUCCESS ||
            !slot.source) {
            if (error) *error = "iplSourceCreate failed while preallocating source pool";
            return {};
        }
        slot.binaural = create_binaural_effect(error);
        if (!slot.binaural) {
            if (error && error->empty()) *error = "failed to preallocate source-pool binaural effect";
            return {};
        }
        iplSourceAdd(slot.source, impl->simulator);
    }
    iplSimulatorCommit(impl->simulator);
    return std::shared_ptr<SteamAudioNativeSourcePool>(new SteamAudioNativeSourcePool(std::move(impl)));
}

bool SteamAudioNativeSourcePool::valid() const noexcept {
    if (!impl_ || !impl_->owner || !impl_->owner->valid() || !impl_->scene || !impl_->simulator ||
        impl_->slots.empty()) return false;
    return std::all_of(impl_->slots.begin(), impl_->slots.end(), [](const Impl::Slot& s) {
        return s.source != nullptr && s.binaural != nullptr;
    });
}

std::string_view SteamAudioNativeSourcePool::backend_name() const noexcept {
    return "Steam Audio native bounded source pool";
}

bool SteamAudioNativeSourcePool::publish_scene(
    const AcousticSnapshot& snapshot, std::span<const SteamAudioNativeMaterial> sourceMaterials,
    std::string* error) {
    if (!valid()) { if (error) *error = "Steam Audio source pool is not valid"; return false; }
    const std::uint64_t generation = snapshot.statistics.sourceGeneration;
    if (generation == 0U || generation <= impl_->sceneGeneration.load(std::memory_order_acquire)) {
        impl_->rejectedSceneGenerations.fetch_add(1U, std::memory_order_relaxed);
        if (error) *error = "Steam Audio pool scene generation is zero or stale";
        return false;
    }
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> materialIndices;
    std::uint8_t maximumMaterial{};
    if (snapshot.surfaces) {
        vertices.reserve(snapshot.surfaces->size() * 8U);
        triangles.reserve(snapshot.surfaces->size() * 12U);
        materialIndices.reserve(snapshot.surfaces->size() * 12U);
        for (const auto& box : *snapshot.surfaces) {
            if (!valid_box(box)) continue;
            maximumMaterial = std::max(maximumMaterial, box.material);
            append_box(box, static_cast<std::int32_t>(box.material), vertices, triangles,
                       materialIndices);
        }
    }
    const std::size_t materialCount = std::max<std::size_t>(1U,
        std::max<std::size_t>(static_cast<std::size_t>(maximumMaterial) + 1U,
                              sourceMaterials.size()));
    std::vector<IPLMaterial> materials(materialCount);
    for (std::size_t i = 0; i < materialCount; ++i)
        materials[i] = to_ipl(i < sourceMaterials.size() ? sourceMaterials[i]
                                                         : SteamAudioNativeMaterial{});
    IPLStaticMesh replacement{};
    if (!triangles.empty()) {
        IPLStaticMeshSettings meshSettings{};
        meshSettings.numVertices = static_cast<IPLint32>(vertices.size());
        meshSettings.numTriangles = static_cast<IPLint32>(triangles.size());
        meshSettings.numMaterials = static_cast<IPLint32>(materials.size());
        meshSettings.vertices = vertices.data();
        meshSettings.triangles = triangles.data();
        meshSettings.materialIndices = materialIndices.data();
        meshSettings.materials = materials.data();
        if (iplStaticMeshCreate(impl_->scene, &meshSettings, &replacement) != IPL_STATUS_SUCCESS ||
            !replacement) { if (error) *error = "iplStaticMeshCreate failed for source pool"; return false; }
        iplStaticMeshAdd(replacement, impl_->scene);
    }
    IPLStaticMesh previous = impl_->mesh;
    if (previous) iplStaticMeshRemove(previous, impl_->scene);
    iplSceneCommit(impl_->scene);
    iplSimulatorSetScene(impl_->simulator, impl_->scene);
    iplSimulatorCommit(impl_->simulator);
    impl_->mesh = replacement;
    if (previous) iplStaticMeshRelease(&previous);
    impl_->sceneGeneration.store(generation, std::memory_order_release);
    impl_->acceptedSceneGenerations.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool SteamAudioNativeSourcePool::simulate_batch(
    const AudioListenerState& listener, std::span<const SteamAudioNativeSourceRequest> requests,
    std::uint64_t sceneGeneration, std::string* error) {
    if (!valid()) { if (error) *error = "Steam Audio source pool is not valid"; return false; }
    if (sceneGeneration == 0U ||
        sceneGeneration != impl_->sceneGeneration.load(std::memory_order_acquire)) {
        if (error) *error = "source-pool generation does not match committed scene";
        return false;
    }
    const auto started = Clock::now();
    std::vector<SteamAudioNativeSourceRequest> selected;
    selected.reserve(std::min(requests.size(), impl_->slots.size()));
    for (const auto& request : requests) {
        if (request.sourceId == 0U || request.transformGeneration == 0U ||
            !std::isfinite(request.perceptualPriority) || !finite_emitter(request.emitter)) {
            impl_->rejectedRequests.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (std::any_of(selected.begin(), selected.end(), [&](const auto& v) {
                return v.sourceId == request.sourceId;
            })) {
            impl_->rejectedRequests.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        selected.push_back(request);
    }
    std::stable_sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) {
        if (a.perceptualPriority != b.perceptualPriority)
            return a.perceptualPriority > b.perceptualPriority;
        const int aq = quality_rank(a.quality), bq = quality_rank(b.quality);
        if (aq != bq) return aq > bq;
        return a.sourceId < b.sourceId;
    });
    if (selected.size() > impl_->slots.size()) selected.resize(impl_->slots.size());

    std::vector<std::size_t> assignment(selected.size(), impl_->slots.size());
    std::vector<bool> used(impl_->slots.size(), false);
    for (std::size_t i = 0; i < selected.size(); ++i) {
        for (std::size_t slot = 0; slot < impl_->slots.size(); ++slot) {
            if (!used[slot] && impl_->slots[slot].sourceId == selected[i].sourceId) {
                assignment[i] = slot; used[slot] = true; break;
            }
        }
    }
    for (std::size_t i = 0; i < selected.size(); ++i) {
        if (assignment[i] != impl_->slots.size()) continue;
        auto it = std::find(used.begin(), used.end(), false);
        if (it == used.end()) break;
        const std::size_t slot = static_cast<std::size_t>(std::distance(used.begin(), it));
        used[slot] = true;
        if (impl_->slots[slot].sourceId != 0U && impl_->slots[slot].sourceId != selected[i].sourceId)
            impl_->slotSteals.fetch_add(1U, std::memory_order_relaxed);
        assignment[i] = slot;
        impl_->slotAssignments.fetch_add(1U, std::memory_order_relaxed);
    }

    IPLSimulationSharedInputs shared{};
    shared.listener = coordinate_space(listener.position, listener.forward, listener.up);
    iplSimulatorSetSharedInputs(impl_->simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);
    for (std::size_t i = 0; i < selected.size(); ++i) {
        auto& slot = impl_->slots[assignment[i]];
        slot.sourceId = selected[i].sourceId;
        slot.priority = selected[i].perceptualPriority;
        slot.quality = selected[i].quality;
        auto inputs = direct_inputs(selected[i].emitter, selected[i].quality, impl_->settings.direct);
        iplSourceSetInputs(slot.source, IPL_SIMULATIONFLAGS_DIRECT, &inputs);
    }
    for (std::size_t slot = 0; slot < impl_->slots.size(); ++slot) {
        if (!used[slot]) { impl_->slots[slot].sourceId = 0U; impl_->slots[slot].priority = 0.0F; }
    }
    iplSimulatorRunDirect(impl_->simulator);

    auto state = std::make_shared<PublishedPoolState>();
    state->sceneGeneration = sceneGeneration;
    state->entries.reserve(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i) {
        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(impl_->slots[assignment[i]].source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
        PublishedPoolEntry entry;
        entry.sourceId = selected[i].sourceId;
        entry.listener = listener;
        entry.emitter = selected[i].emitter;
        entry.quality = selected[i].quality;
        entry.perceptualPriority = selected[i].perceptualPriority;
        entry.transformGeneration = selected[i].transformGeneration;
        entry.slot = assignment[i];
        entry.path = direct_path_from_outputs(outputs, listener, selected[i].emitter);
        state->entries.push_back(entry);
    }
    impl_->published.store(std::move(state), std::memory_order_release);
    impl_->activeSources.store(selected.size(), std::memory_order_release);
    impl_->simulationBatches.fetch_add(1U, std::memory_order_relaxed);
    impl_->simulatedSources.fetch_add(selected.size(), std::memory_order_relaxed);
    impl_->lastBatchMilliseconds.store(
        std::chrono::duration<double, std::milli>(Clock::now() - started).count(),
        std::memory_order_relaxed);
    return true;
}

bool SteamAudioNativeSourcePool::process_source(
    std::uint64_t sourceId, std::span<const float> monoInput,
    std::span<float> interleavedOutput, AudioVec3 listenerRelativeDirection,
    float spatialBlend) noexcept {
    if (!valid() || sourceId == 0U) return false;
    const auto state = impl_->published.load(std::memory_order_acquire);
    if (!state) {
        impl_->hrtfProcessMisses.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    const auto found = std::find_if(state->entries.begin(), state->entries.end(),
        [sourceId](const PublishedPoolEntry& entry) { return entry.sourceId == sourceId; });
    if (found == state->entries.end() || found->slot >= impl_->slots.size()) {
        impl_->hrtfProcessMisses.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    auto& effect = impl_->slots[found->slot].binaural;
    if (!effect || !effect->process_mono(monoInput, interleavedOutput,
                                         listenerRelativeDirection, spatialBlend)) {
        impl_->hrtfProcessMisses.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    impl_->hrtfProcessCalls.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

SteamAudioDirectPath SteamAudioNativeSourcePool::query_direct(
    const AudioListenerState& listener, const AudioEmitterState& emitter,
    SteamAudioQuality quality) const noexcept {
    if (!valid()) return invalid_direct_path();
    const auto state = impl_->published.load(std::memory_order_acquire);
    if (!state || state->sceneGeneration != impl_->sceneGeneration.load(std::memory_order_acquire)) {
        impl_->queryMisses.fetch_add(1U, std::memory_order_relaxed);
        return invalid_direct_path();
    }
    const float tolerance = impl_->settings.direct.sourceMatchToleranceMeters;
    const float toleranceSquared = tolerance * tolerance;
    const PublishedPoolEntry* best{};
    float bestDistance = std::numeric_limits<float>::infinity();
    for (const auto& entry : state->entries) {
        if (entry.quality != quality ||
            distance_squared(entry.listener.position, listener.position) > toleranceSquared)
            continue;
        const float d = distance_squared(entry.emitter.position, emitter.position);
        if (d <= toleranceSquared && d < bestDistance) { best = &entry; bestDistance = d; }
    }
    if (!best) {
        impl_->queryMisses.fetch_add(1U, std::memory_order_relaxed);
        return invalid_direct_path();
    }
    impl_->queryHits.fetch_add(1U, std::memory_order_relaxed);
    return best->path;
}

SteamAudioNativeSourcePoolMetrics SteamAudioNativeSourcePool::metrics() const noexcept {
    if (!impl_) return {};
    return {impl_->acceptedSceneGenerations.load(std::memory_order_relaxed),
            impl_->rejectedSceneGenerations.load(std::memory_order_relaxed),
            impl_->simulationBatches.load(std::memory_order_relaxed),
            impl_->simulatedSources.load(std::memory_order_relaxed),
            impl_->rejectedRequests.load(std::memory_order_relaxed),
            impl_->slotAssignments.load(std::memory_order_relaxed),
            impl_->slotSteals.load(std::memory_order_relaxed),
            impl_->queryHits.load(std::memory_order_relaxed),
            impl_->queryMisses.load(std::memory_order_relaxed),
            impl_->hrtfProcessCalls.load(std::memory_order_relaxed),
            impl_->hrtfProcessMisses.load(std::memory_order_relaxed),
            impl_->sceneGeneration.load(std::memory_order_acquire),
            impl_->activeSources.load(std::memory_order_acquire),
            impl_->slots.size(),
            impl_->lastBatchMilliseconds.load(std::memory_order_relaxed)};
}

} // namespace dve::audio
