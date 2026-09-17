#pragma once

#include <cstdint>

namespace dve::audio {

struct AudioVec3 {
    float x{};
    float y{};
    float z{};
};

struct AudioListenerState {
    AudioVec3 position{};
    AudioVec3 velocity{};
    AudioVec3 forward{0.0F, 0.0F, -1.0F};
    AudioVec3 up{0.0F, 1.0F, 0.0F};
};

struct AudioEmitterState {
    AudioVec3 position{};
    AudioVec3 velocity{};
    AudioVec3 forward{0.0F, 0.0F, -1.0F};
    float radiusMeters{0.1F};
    float minDistanceMeters{1.0F};
    float maxDistanceMeters{100.0F};
    float directivity{};
    float occlusion{};
    float transmission{1.0F};
    float reverbSend{0.12F};
};

struct SpatializationResult {
    float leftGain{0.70710678F};
    float rightGain{0.70710678F};
    float distanceGain{1.0F};
    float dopplerRatio{1.0F};
    float lowPassHertz{20000.0F};
    float reverbSend{};
    float propagationDelaySeconds{};
};

class IAudioSpatializer {
public:
    virtual ~IAudioSpatializer() = default;
    [[nodiscard]] virtual SpatializationResult spatialize(const AudioListenerState& listener,
                                                           const AudioEmitterState& emitter) const noexcept = 0;
};

// Low-cost equal-power stereo spatializer used by the deterministic runtime and as the
// fallback when no HRTF/propagation middleware is active. It performs no allocation or locks.
class AnalyticSpatializer final : public IAudioSpatializer {
public:
    [[nodiscard]] SpatializationResult spatialize(const AudioListenerState& listener,
                                                   const AudioEmitterState& emitter) const noexcept override;
};

} // namespace dve::audio
