#pragma once

#include <cstdint>

#include "dve/types.hpp"

namespace dve {

// Value and analytic spatial derivative of a continuous 3D noise field.
// The gradient is in units of value per input-coordinate unit.
struct NoiseSample3 {
    float value{};
    Float3 gradient{};
};

struct FractalNoiseSettings {
    float frequency{1.0F};
    std::uint32_t octaves{1U};
    float lacunarity{2.0F};
    float persistence{0.5F};
    std::uint32_t seed{};
};

// Quintic-smoothed lattice value noise. Both value and gradient are analytic; the quintic
// interpolation has zero first and second derivatives at lattice boundaries.
[[nodiscard]] NoiseSample3 value_noise_3d(Float3 position, std::uint32_t seed = 0U) noexcept;

// Normalized fractal sum. The chain rule scales each octave's gradient by its frequency.
[[nodiscard]] NoiseSample3 fractal_noise_3d(
    Float3 position, const FractalNoiseSettings& settings) noexcept;

[[nodiscard]] bool validate_fractal_noise_settings(
    const FractalNoiseSettings& settings) noexcept;

} // namespace dve
