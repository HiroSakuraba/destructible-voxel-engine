#include "dve/procedural_noise.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace dve {
namespace {

struct AxisWeights {
    float low{};
    float high{};
    float lowDerivative{};
    float highDerivative{};
};

AxisWeights axis_weights(float fraction) noexcept {
    const float t2 = fraction * fraction;
    const float t3 = t2 * fraction;
    const float t4 = t3 * fraction;
    const float t5 = t4 * fraction;
    const float smooth = 6.0F * t5 - 15.0F * t4 + 10.0F * t3;
    const float derivative = 30.0F * t2 * (fraction - 1.0F) * (fraction - 1.0F);
    return {1.0F - smooth, smooth, -derivative, derivative};
}

std::uint32_t lattice_hash(std::int64_t x, std::int64_t y, std::int64_t z,
                           std::uint32_t seed) noexcept {
    std::uint32_t hash = seed ^ 0x9E3779B9U;
    hash ^= static_cast<std::uint32_t>(x) + 0x85EBCA6BU + (hash << 6U) + (hash >> 2U);
    hash ^= static_cast<std::uint32_t>(y) + 0xC2B2AE35U + (hash << 6U) + (hash >> 2U);
    hash ^= static_cast<std::uint32_t>(z) + 0x27D4EB2FU + (hash << 6U) + (hash >> 2U);
    hash ^= hash >> 16U;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15U;
    hash *= 0x846CA68BU;
    hash ^= hash >> 16U;
    return hash;
}

float lattice_value(std::int64_t x, std::int64_t y, std::int64_t z,
                    std::uint32_t seed) noexcept {
    // Use 24 hash bits so conversion to float is deterministic and exactly reproducible.
    constexpr float inverse = 1.0F / 8388607.5F;
    return static_cast<float>(lattice_hash(x, y, z, seed) >> 8U) * inverse - 1.0F;
}

} // namespace

NoiseSample3 value_noise_3d(Float3 position, std::uint32_t seed) noexcept {
    constexpr float coordinateLimit = 1048576.0F;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        std::abs(position.x) >= coordinateLimit || std::abs(position.y) >= coordinateLimit ||
        std::abs(position.z) >= coordinateLimit)
        return {};

    const auto ix = static_cast<std::int64_t>(std::floor(position.x));
    const auto iy = static_cast<std::int64_t>(std::floor(position.y));
    const auto iz = static_cast<std::int64_t>(std::floor(position.z));
    const AxisWeights wx = axis_weights(position.x - static_cast<float>(ix));
    const AxisWeights wy = axis_weights(position.y - static_cast<float>(iy));
    const AxisWeights wz = axis_weights(position.z - static_cast<float>(iz));
    const std::array<float, 2> xWeights{wx.low, wx.high};
    const std::array<float, 2> yWeights{wy.low, wy.high};
    const std::array<float, 2> zWeights{wz.low, wz.high};
    const std::array<float, 2> xDerivatives{wx.lowDerivative, wx.highDerivative};
    const std::array<float, 2> yDerivatives{wy.lowDerivative, wy.highDerivative};
    const std::array<float, 2> zDerivatives{wz.lowDerivative, wz.highDerivative};

    NoiseSample3 result{};
    for (std::int64_t z = 0; z < 2; ++z) {
        for (std::int64_t y = 0; y < 2; ++y) {
            for (std::int64_t x = 0; x < 2; ++x) {
                const float value = lattice_value(ix + x, iy + y, iz + z, seed);
                result.value += value * xWeights[static_cast<std::size_t>(x)] *
                    yWeights[static_cast<std::size_t>(y)] * zWeights[static_cast<std::size_t>(z)];
                result.gradient.x += value * xDerivatives[static_cast<std::size_t>(x)] *
                    yWeights[static_cast<std::size_t>(y)] * zWeights[static_cast<std::size_t>(z)];
                result.gradient.y += value * xWeights[static_cast<std::size_t>(x)] *
                    yDerivatives[static_cast<std::size_t>(y)] * zWeights[static_cast<std::size_t>(z)];
                result.gradient.z += value * xWeights[static_cast<std::size_t>(x)] *
                    yWeights[static_cast<std::size_t>(y)] * zDerivatives[static_cast<std::size_t>(z)];
            }
        }
    }
    return result;
}

bool validate_fractal_noise_settings(const FractalNoiseSettings& settings) noexcept {
    return std::isfinite(settings.frequency) && settings.frequency > 0.0F &&
        settings.frequency <= 65536.0F && settings.octaves >= 1U && settings.octaves <= 12U &&
        std::isfinite(settings.lacunarity) && settings.lacunarity >= 1.0F &&
        settings.lacunarity <= 4.0F && std::isfinite(settings.persistence) &&
        settings.persistence >= 0.0F && settings.persistence <= 1.0F;
}

NoiseSample3 fractal_noise_3d(Float3 position, const FractalNoiseSettings& settings) noexcept {
    if (!validate_fractal_noise_settings(settings)) return {};
    NoiseSample3 result{};
    float frequency = settings.frequency;
    float amplitude = 1.0F;
    float amplitudeSum = 0.0F;
    for (std::uint32_t octave = 0; octave < settings.octaves; ++octave) {
        const NoiseSample3 sample = value_noise_3d(
            {position.x * frequency, position.y * frequency, position.z * frequency},
            settings.seed + octave * 0x9E3779B9U);
        result.value += amplitude * sample.value;
        result.gradient.x += amplitude * frequency * sample.gradient.x;
        result.gradient.y += amplitude * frequency * sample.gradient.y;
        result.gradient.z += amplitude * frequency * sample.gradient.z;
        amplitudeSum += amplitude;
        amplitude *= settings.persistence;
        frequency *= settings.lacunarity;
    }
    if (amplitudeSum > 0.0F) {
        const float inverse = 1.0F / amplitudeSum;
        result.value *= inverse;
        result.gradient.x *= inverse;
        result.gradient.y *= inverse;
        result.gradient.z *= inverse;
    }
    return result;
}

} // namespace dve
