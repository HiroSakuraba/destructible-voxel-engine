#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "dve/audio/acoustic_runtime.hpp"
#include "dve/audio/steam_audio.hpp"

namespace dve::audio {

struct SteamAudioNativeSettings {
    std::uint32_t sampleRate{48000};
    std::size_t frameSize{256};
    std::string sofaFile;
    bool bilinearInterpolation{};
};

struct SteamAudioNativeMaterial {
    std::array<float, 3> absorption{0.10F, 0.20F, 0.30F};
    float scattering{0.05F};
    std::array<float, 3> transmission{0.10F, 0.05F, 0.03F};
};

struct SteamAudioNativeDirectSettings {
    std::int32_t maximumOcclusionSamples{32};
    std::int32_t normalOcclusionSamples{8};
    std::int32_t importantOcclusionSamples{16};
    std::int32_t heroOcclusionSamples{32};
    std::int32_t transmissionRays{4};
    float sourceMatchToleranceMeters{0.25F};
};


struct SteamAudioNativeSourceRequest {
    std::uint64_t sourceId{};
    AudioEmitterState emitter{};
    SteamAudioQuality quality{SteamAudioQuality::Normal};
    float perceptualPriority{};
    std::uint64_t transformGeneration{};
};

struct SteamAudioNativeSourcePoolSettings {
    std::size_t maximumSources{16};
    SteamAudioNativeDirectSettings direct{};
};

struct SteamAudioNativeSourcePoolMetrics {
    std::uint64_t acceptedSceneGenerations{};
    std::uint64_t rejectedSceneGenerations{};
    std::uint64_t simulationBatches{};
    std::uint64_t simulatedSources{};
    std::uint64_t rejectedRequests{};
    std::uint64_t slotAssignments{};
    std::uint64_t slotSteals{};
    std::uint64_t queryHits{};
    std::uint64_t queryMisses{};
    std::uint64_t hrtfProcessCalls{};
    std::uint64_t hrtfProcessMisses{};
    std::uint64_t lastSceneGeneration{};
    std::size_t activeSources{};
    std::size_t capacity{};
    double lastBatchMilliseconds{};
};

struct SteamAudioNativeDirectMetrics {
    std::uint64_t acceptedSceneGenerations{};
    std::uint64_t rejectedSceneGenerations{};
    std::uint64_t simulationRuns{};
    std::uint64_t queryHits{};
    std::uint64_t queryMisses{};
    std::uint64_t lastSceneGeneration{};
    std::uint64_t lastSimulatedGeneration{};
    std::uint64_t vertices{};
    std::uint64_t triangles{};
    double lastSceneBuildMilliseconds{};
    double lastSimulationMilliseconds{};
};

class SteamAudioNativeDirectSimulator;
class SteamAudioNativeSourcePool;

// PIMPL keeps phonon.h out of the portable engine boundary. This class exists only in the
// optional dve_audio_spatial_steam target; the core mixer and event runtime remain SDK-neutral.
class SteamAudioNativeContext : public std::enable_shared_from_this<SteamAudioNativeContext> {
public:
    ~SteamAudioNativeContext();
    SteamAudioNativeContext(const SteamAudioNativeContext&) = delete;
    SteamAudioNativeContext& operator=(const SteamAudioNativeContext&) = delete;

    [[nodiscard]] static std::shared_ptr<SteamAudioNativeContext> create(
        const SteamAudioNativeSettings& settings, std::string* error = nullptr);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] std::size_t frame_size() const noexcept;
    [[nodiscard]] std::unique_ptr<ISpatialAudioBlockEffect> create_binaural_effect(
        std::string* error = nullptr);
    [[nodiscard]] std::shared_ptr<SteamAudioNativeDirectSimulator> create_direct_simulator(
        const SteamAudioNativeDirectSettings& settings = {}, std::string* error = nullptr);
    [[nodiscard]] std::shared_ptr<SteamAudioNativeSourcePool> create_source_pool(
        const SteamAudioNativeSourcePoolSettings& settings = {}, std::string* error = nullptr);

private:
    friend class SteamAudioNativeDirectSimulator;
    friend class SteamAudioNativeSourcePool;
    struct Impl;
    explicit SteamAudioNativeContext(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// A one-hero-source direct-simulation vertical slice. Scene conversion and IPLSimulator execution
// occur only on an acoustic/control worker through publish_scene() and simulate_direct(). The
// audio callback reaches only query_direct(), which atomically reads the last immutable result.
class SteamAudioNativeDirectSimulator final : public ISteamAudioSimulationBackend {
public:
    ~SteamAudioNativeDirectSimulator() override;
    SteamAudioNativeDirectSimulator(const SteamAudioNativeDirectSimulator&) = delete;
    SteamAudioNativeDirectSimulator& operator=(const SteamAudioNativeDirectSimulator&) = delete;

    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] std::string_view backend_name() const noexcept override;

    bool publish_scene(const AcousticSnapshot& snapshot,
                       std::span<const SteamAudioNativeMaterial> materials = {},
                       std::string* error = nullptr);
    bool simulate_direct(const AudioListenerState& listener,
                         const AudioEmitterState& emitter,
                         SteamAudioQuality quality,
                         std::uint64_t sourceGeneration,
                         std::string* error = nullptr);

    [[nodiscard]] SteamAudioDirectPath query_direct(
        const AudioListenerState& listener, const AudioEmitterState& emitter,
        SteamAudioQuality quality) const noexcept override;
    [[nodiscard]] SteamAudioNativeDirectMetrics metrics() const noexcept;

private:
    friend class SteamAudioNativeContext;
    struct Impl;
    explicit SteamAudioNativeDirectSimulator(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};


// Fixed-capacity direct-simulation pool. All IPLSource objects are created during setup.
// A worker submits a bounded request batch and one immutable result array is published for
// callback-side query. Slot selection and stealing are deterministic.
class SteamAudioNativeSourcePool final : public ISteamAudioSimulationBackend {
public:
    ~SteamAudioNativeSourcePool() override;
    SteamAudioNativeSourcePool(const SteamAudioNativeSourcePool&) = delete;
    SteamAudioNativeSourcePool& operator=(const SteamAudioNativeSourcePool&) = delete;

    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] std::string_view backend_name() const noexcept override;
    bool publish_scene(const AcousticSnapshot& snapshot,
                       std::span<const SteamAudioNativeMaterial> materials = {},
                       std::string* error = nullptr);
    bool simulate_batch(const AudioListenerState& listener,
                        std::span<const SteamAudioNativeSourceRequest> requests,
                        std::uint64_t sceneGeneration,
                        std::string* error = nullptr);
    bool process_source(std::uint64_t sourceId,
                        std::span<const float> monoInput,
                        std::span<float> interleavedOutput,
                        AudioVec3 listenerRelativeDirection,
                        float spatialBlend) noexcept;
    [[nodiscard]] SteamAudioDirectPath query_direct(
        const AudioListenerState& listener, const AudioEmitterState& emitter,
        SteamAudioQuality quality) const noexcept override;
    [[nodiscard]] SteamAudioNativeSourcePoolMetrics metrics() const noexcept;

private:
    friend class SteamAudioNativeContext;
    struct Impl;
    explicit SteamAudioNativeSourcePool(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace dve::audio
