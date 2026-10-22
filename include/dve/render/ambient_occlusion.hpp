#pragma once

// Ambient occlusion for the CPU reference renderers, exactly as the GPU passes compute it
// (generate_ao_rays.hlsl + resolve_ao.hlsl):
//   ray i of n leaves point + normal x max(1e-3, shadow bias) along
//     CosineWeightedHemisphereSample(normal, RotatedHammersley2D(i, n, PcgHash(pixel ^ frameSeed)))
//   out to the AO max distance; a hit at distance d occludes (1 - d / max)^2;
//   visibility = 1 - mean occlusion.
// Distances are in the caller's world units (voxel units for the voxel reference), converted
// from the environment's metres with metersPerUnit.

#include <cstdint>
#include <functional>
#include <optional>

#include "dve/render_environment.hpp"
#include "dve/types.hpp"

namespace dve::render {

// The nearest hit distance along a ray (unit direction, world units), or nullopt.
using AmbientOcclusionRay = std::function<std::optional<float>(Float3 origin, Float3 direction, float maximumDistance)>;

[[nodiscard]] Float3 ambient_occlusion_ray_direction(Float3 normal, std::uint32_t sampleIndex, std::uint32_t sampleCount,
                                                     std::uint32_t pixelIndex, std::uint32_t frameSeed) noexcept;
[[nodiscard]] float ambient_occlusion_origin_bias(const RenderEnvironment& environment, float metersPerUnit) noexcept;
// 1 when the environment's samples or max distance are 0.
[[nodiscard]] float reference_ambient_occlusion(Float3 point, Float3 normal, std::uint32_t pixelIndex,
                                                std::uint32_t frameSeed, const RenderEnvironment& environment,
                                                float metersPerUnit, const AmbientOcclusionRay& nearest);

} // namespace dve::render
