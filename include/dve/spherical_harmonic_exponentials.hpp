// Factorized log-space SH-exponential representation for glossy environment probes.
// Inspired by Silvennoinen et al., CGF/HPG 2025, DOI:10.1111/cgf.70219.
// E0(r, h_r, alpha) = exp(sum(P_lm Y_lm(r) vMF_l(1/alpha))
//                         + sum(Q_lm Y_lm(h_r) vMF_l(1/alpha))).
// P uses degree 4 (25), Q degree 2 excluding its redundant DC term (8).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "dve/environment_lighting.hpp"

namespace dve {
constexpr std::size_t kSheCoefficientCount = 33;
constexpr std::size_t kShePackedHalfCount = 104; // 99 RGB coefficients plus padding = 208 B
constexpr float kSheMinimumAlpha = 0.25F;

struct SheProbeCoefficients {
    // Channel-major; indices 0..24 are P; 25..32 are Q degree 1 and 2.
    std::array<std::array<float, kSheCoefficientCount>, 3> rgb{};
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct ShePackedProbe {
    // Coefficient-major RGB half-floats, followed by five zero padding halfs.
    // GPU buffers can reinterpret this as 52 packed uint32 values (208 bytes).
    std::array<std::uint16_t, kShePackedHalfCount> half{};
};
static_assert(sizeof(ShePackedProbe) == 208);

struct SheFitSample {
    Float3 normal{0, 1, 0};
    Float3 viewDirection{0, 0, 1}; // points from surface to camera
    float alpha{0.5F};             // GGX microfacet alpha = perceptual roughness squared
    Float3 baseReflectance{1, 1, 1}; // irradiance-independent Fresnel=1 GGX integral E0
};

struct SheFitResult {
    SheProbeCoefficients coefficients;
    float logRmse{};
    std::uint32_t fittedSamples{};
};

struct SheBakeSettings {
    std::uint32_t trainingSamples{2048};
    std::uint32_t ggxSamplesPerTrainingPoint{96};
    float regularization{1.0e-3F};
    [[nodiscard]] bool validate() const noexcept;
};

// Fitting is an offline CPU reference, not intended for per-frame invocation.
// Solves log(E0) with Tikhonov-regularized least squares; samples must cover
// directions, N.V, and alpha. A small diagonal regularizer prevents rank collapse.
[[nodiscard]] std::optional<SheFitResult> fit_she_probe(
    std::span<const SheFitSample> samples, float regularization = 1.0e-3F);

// Offline GGX Monte Carlo target generation plus log-space fitting from DVE cubemaps.
[[nodiscard]] std::optional<SheFitResult> bake_she_probe(
    const EnvironmentCube& source, const SheBakeSettings& settings = {});

// Nullopt indicates an unsupported alpha, invalid directions, or invalid coefficients.
// The calling renderer must use its existing split-sum path in that case.
[[nodiscard]] std::optional<Float3> evaluate_she_base_reflectance(
    const SheProbeCoefficients& probe, Float3 normal, Float3 viewDirection,
    float alpha) noexcept;

[[nodiscard]] ShePackedProbe pack_she_probe_half(const SheProbeCoefficients& probe) noexcept;
[[nodiscard]] SheProbeCoefficients unpack_she_probe_half(const ShePackedProbe& packed) noexcept;

} // namespace dve
