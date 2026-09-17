#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "dve/audio/spatializer.hpp"

namespace dve::audio {

enum class SteamAudioQuality : std::uint8_t { Background, Normal, Important, Hero };
enum class AudioOutputMode : std::uint8_t { StereoSpeakers, HeadphonesHrtf, Surround51, Surround71 };

struct SteamAudioDirectPath {
    float distanceAttenuation{1.0F};
    float airAbsorptionLow{1.0F};
    float airAbsorptionMid{1.0F};
    float airAbsorptionHigh{1.0F};
    float directivity{1.0F};
    float occlusion{};
    float transmissionLow{1.0F};
    float transmissionMid{1.0F};
    float transmissionHigh{1.0F};
    float reverbSend{};
    float propagationDelaySeconds{};
};

// SDK-neutral simulation contract. A production implementation wraps IPLSimulator/IPLSource on
// acoustic workers; deterministic tests can inject a fake without Steam Audio binaries.
class ISteamAudioSimulationBackend {
public:
    virtual ~ISteamAudioSimulationBackend() = default;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual SteamAudioDirectPath query_direct(
        const AudioListenerState& listener, const AudioEmitterState& emitter,
        SteamAudioQuality quality) const noexcept = 0;
};

struct SteamAudioSpatializerMetrics {
    std::uint64_t backendQueries{};
    std::uint64_t fallbackQueries{};
    std::uint64_t invalidResults{};
};

// Converts published Steam Audio simulation results into the current mixer direct-path contract.
// HRTF sample processing is intentionally a separate fixed-block effect because Steam Audio
// binaural effects maintain per-source state and require a fixed frame size.
class SteamAudioSpatializer final : public IAudioSpatializer {
public:
    explicit SteamAudioSpatializer(std::shared_ptr<const ISteamAudioSimulationBackend> backend,
                                   SteamAudioQuality quality = SteamAudioQuality::Normal) noexcept;
    void set_quality(SteamAudioQuality quality) noexcept;
    [[nodiscard]] SteamAudioQuality quality() const noexcept;
    [[nodiscard]] SpatializationResult spatialize(const AudioListenerState& listener,
                                                   const AudioEmitterState& emitter) const noexcept override;
    [[nodiscard]] SteamAudioSpatializerMetrics metrics() const noexcept;
private:
    std::shared_ptr<const ISteamAudioSimulationBackend> backend_;
    std::atomic<SteamAudioQuality> quality_;
    AnalyticSpatializer fallback_;
    mutable std::atomic<std::uint64_t> backendQueries_{};
    mutable std::atomic<std::uint64_t> fallbackQueries_{};
    mutable std::atomic<std::uint64_t> invalidResults_{};
};

// Fixed-block interface for IPLBinauralEffect or speaker decoding. The audio callback owns one
// effect instance per physically mixed hero/important source; effects are never shared across
// callback threads.
class ISpatialAudioBlockEffect {
public:
    virtual ~ISpatialAudioBlockEffect() = default;
    [[nodiscard]] virtual std::size_t frame_size() const noexcept = 0;
    [[nodiscard]] virtual AudioOutputMode output_mode() const noexcept = 0;
    virtual bool process_mono(std::span<const float> monoInput,
                              std::span<float> interleavedOutput,
                              AudioVec3 listenerRelativeDirection,
                              float spatialBlend) noexcept = 0;
    virtual void reset() noexcept = 0;
};

} // namespace dve::audio
