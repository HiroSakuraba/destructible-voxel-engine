#include "dve/audio/ambisonics.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace dve::audio {
namespace {

AudioVec3 normalize(AudioVec3 value) noexcept {
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (!(lengthSquared > 1.0e-12F) || !std::isfinite(lengthSquared)) return {0.0F, 0.0F, 1.0F};
    const float scale = 1.0F / std::sqrt(lengthSquared);
    return {value.x * scale, value.y * scale, value.z * scale};
}

std::array<float, 9> basis(AudioVec3 direction) noexcept {
    const AudioVec3 d = normalize(direction);
    constexpr float kSqrt3 = 1.7320508075688772F;
    std::array<float, 9> result{};
    result[0] = 1.0F;
    // DVE coordinate order: lateral/right, vertical/up, forward.
    result[1] = d.x;
    result[2] = d.y;
    result[3] = d.z;
    result[4] = kSqrt3 * d.x * d.z;
    result[5] = kSqrt3 * d.y * d.z;
    result[6] = 0.5F * (3.0F * d.y * d.y - 1.0F);
    result[7] = kSqrt3 * d.x * d.y;
    result[8] = 0.5F * kSqrt3 * (d.x * d.x - d.z * d.z);
    return result;
}

} // namespace

AmbisonicFieldProcessor::AmbisonicFieldProcessor(std::size_t order) noexcept
    : order_(std::min<std::size_t>(order, 2U)) {}

bool AmbisonicFieldProcessor::encode_add(
    std::span<const float> monoInput, std::span<float> interleavedField,
    AudioVec3 direction, float gain) const noexcept {
    const std::size_t channelCount = channels();
    if (channelCount == 0U || interleavedField.size() != monoInput.size() * channelCount ||
        !std::isfinite(gain)) return false;
    const auto coefficients = basis(direction);
    for (std::size_t frame = 0; frame < monoInput.size(); ++frame) {
        const float sample = monoInput[frame] * gain;
        for (std::size_t channel = 0; channel < channelCount; ++channel)
            interleavedField[frame * channelCount + channel] += sample * coefficients[channel];
    }
    return true;
}

bool AmbisonicFieldProcessor::decode_stereo(
    std::span<const float> interleavedField,
    std::span<float> interleavedStereo) const noexcept {
    const std::size_t channelCount = channels();
    if (channelCount == 0U || interleavedStereo.size() % 2U != 0U ||
        interleavedField.size() != (interleavedStereo.size() / 2U) * channelCount) return false;
    // Virtual speakers 30 degrees left/right, slightly diffuse second-order contribution.
    const auto leftBasis = basis({-0.5F, 0.0F, 0.8660254F});
    const auto rightBasis = basis({0.5F, 0.0F, 0.8660254F});
    const float normalization = order_ == 0U ? 0.5F : (order_ == 1U ? 0.25F : 0.16F);
    const std::size_t frames = interleavedStereo.size() / 2U;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float left{};
        float right{};
        for (std::size_t channel = 0; channel < channelCount; ++channel) {
            const float value = interleavedField[frame * channelCount + channel];
            left += value * leftBasis[channel];
            right += value * rightBasis[channel];
        }
        interleavedStereo[frame * 2U] = left * normalization;
        interleavedStereo[frame * 2U + 1U] = right * normalization;
    }
    return true;
}

} // namespace dve::audio
