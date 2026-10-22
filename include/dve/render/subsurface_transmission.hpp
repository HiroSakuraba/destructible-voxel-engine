#pragma once

// How much sunlight scatters through a Subsurface slab, from the random-walk ground truth
// (subsurface_random_walk.hpp), baked into a table by apps/dve_bake_subsurface_transmission.cpp.
//
// subsurface_slab_transmission(A, t/d, mu): the share of collimated light arriving at cosine mu on
// one face of a slab t thick (in Burley scatter distances d) that leaves the other face after
// scattering at least once. The medium is Chiang et al.'s for albedo A (isotropic,
// index-matched). Light that crosses without scattering is not included: it is a straight beam
// (the sun seen through the slab), not a glow.
//
// Grid: mu = 0.2, 0.3, ..., 1.0; A = 0, 0.05, ..., 1.0; t/d = 16 (k/32)^2 for k = 0..32. Trilinear
// in (mu, A, sqrt(t/d)); past t/d = 16 the last two samples' exponential decay continues. The GPU
// (shaders/common/subsurface.hlsli, SubsurfaceSlabTransmission) reads the same numbers.

#include <cstddef>

namespace dve::render {

inline constexpr std::size_t kSubsurfaceTransmissionCosines = 9;
inline constexpr std::size_t kSubsurfaceTransmissionAlbedos = 21;
inline constexpr std::size_t kSubsurfaceTransmissionThicknesses = 33;

[[nodiscard]] constexpr float subsurface_transmission_cosine(std::size_t i) noexcept {
    return 0.2F + 0.1F * static_cast<float>(i);
}
[[nodiscard]] constexpr float subsurface_transmission_albedo(std::size_t i) noexcept {
    return 0.05F * static_cast<float>(i);
}
[[nodiscard]] constexpr float subsurface_transmission_thickness(std::size_t i) noexcept {
    const float u = static_cast<float>(i) / 32.0F;
    return 16.0F * u * u;
}

// The baked entry [cosine][albedo][thickness].
[[nodiscard]] float subsurface_transmission_entry(std::size_t cosine, std::size_t albedo, std::size_t thickness) noexcept;

[[nodiscard]] float subsurface_slab_transmission(float albedo, float thicknessOverScatterDistance,
                                                 float cosIncidence) noexcept;

} // namespace dve::render
