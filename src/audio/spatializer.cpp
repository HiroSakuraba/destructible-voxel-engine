#include "dve/audio/spatializer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dve::audio {
namespace {

AudioVec3 subtract(AudioVec3 a, AudioVec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(AudioVec3 a, AudioVec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
AudioVec3 cross(AudioVec3 a, AudioVec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(AudioVec3 value) noexcept { return std::sqrt(std::max(0.0F, dot(value, value))); }
AudioVec3 normalized(AudioVec3 value, AudioVec3 fallback) noexcept {
    const float magnitude = length(value);
    if (magnitude <= 1.0e-6F) return fallback;
    return {value.x / magnitude, value.y / magnitude, value.z / magnitude};
}

} // namespace

SpatializationResult AnalyticSpatializer::spatialize(const AudioListenerState& listener,
                                                       const AudioEmitterState& emitter) const noexcept {
    constexpr float speedOfSound = 343.0F;
    const AudioVec3 displacement = subtract(emitter.position, listener.position);
    const float distance = length(displacement);
    const AudioVec3 direction = normalized(displacement, listener.forward);
    const AudioVec3 forward = normalized(listener.forward, {0.0F, 0.0F, -1.0F});
    const AudioVec3 up = normalized(listener.up, {0.0F, 1.0F, 0.0F});
    const AudioVec3 right = normalized(cross(forward, up), {1.0F, 0.0F, 0.0F});

    const float pan = std::clamp(dot(direction, right), -1.0F, 1.0F);
    const float angle = (pan + 1.0F) * (std::numbers::pi_v<float> * 0.25F);

    const float minDistance = std::max(0.05F, emitter.minDistanceMeters);
    const float maxDistance = std::max(minDistance, emitter.maxDistanceMeters);
    const float clampedDistance = std::clamp(distance - std::max(0.0F, emitter.radiusMeters), minDistance, maxDistance);
    const float inverseDistance = minDistance / clampedDistance;
    const float rangeFade = 1.0F - std::clamp((distance - maxDistance * 0.85F) /
                                              std::max(0.001F, maxDistance * 0.15F), 0.0F, 1.0F);
    const float obstruction = std::clamp(emitter.occlusion, 0.0F, 1.0F);
    const float transmission = std::clamp(emitter.transmission, 0.0F, 1.0F);

    const float listenerRadial = dot(listener.velocity, direction);
    const float emitterRadial = dot(emitter.velocity, direction);
    const float doppler = std::clamp((speedOfSound + listenerRadial) /
                                     std::max(1.0F, speedOfSound + emitterRadial), 0.5F, 2.0F);

    SpatializationResult result;
    result.leftGain = std::cos(angle);
    result.rightGain = std::sin(angle);
    result.distanceGain = inverseDistance * rangeFade * ((1.0F - obstruction) + obstruction * transmission * 0.35F);
    result.dopplerRatio = doppler;
    result.lowPassHertz = 20000.0F - obstruction * (18000.0F - transmission * 6000.0F);
    result.reverbSend = std::clamp(emitter.reverbSend + obstruction * 0.20F, 0.0F, 1.0F);
    result.propagationDelaySeconds = distance / speedOfSound;
    return result;
}

} // namespace dve::audio
