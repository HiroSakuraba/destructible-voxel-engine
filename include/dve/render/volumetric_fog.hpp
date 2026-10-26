#pragma once

// Volumetric height fog with light shafts (RenderEnvironment::volumetricFog*).
//
// Along each pixel's ray, from the camera to the surface (or volumetricFogMaxDistanceMeters,
// whichever is nearer; sky pixels to the maximum), the march takes volumetricFogSamples equal
// steps. Each step's optical depth is exact for the exponential height profile, and the step
// adds
//     T_before x (1 - e^-tau) x albedo x [pi x HG(g, view . sun) x E_sun x visibility + ambient]
// which is the exact integral when the light is constant across the step. visibility comes from
// a shadow ray toward the sun at a jittered point in the step (shadow strength applied; 1 with
// shadows off). E_sun is the sun at the ground (through the physical atmosphere when on);
// ambient is the sky's light, the mean of the sky irradiance from above and below, scaled by
// globalIlluminationIntensity (none with GI off), and only where a ray straight up from the same
// point reaches the open sky. Values are in the CPU renderers' convention (pi x radiance).

#include <cstdint>

#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render_environment.hpp"

namespace dve::render {

// Extinction (per metre) at a height (metres, world y x metres per unit).
[[nodiscard]] float volumetric_fog_density(const RenderEnvironment& environment, float heightMeters) noexcept;
// Optical depth along a straight segment starting at a height, climbing directionY metres per
// metre of length (closed form).
[[nodiscard]] double volumetric_fog_optical_depth(const RenderEnvironment& environment, float startHeightMeters, float directionY,
                                                  float lengthMeters) noexcept;
[[nodiscard]] float henyey_greenstein_phase(float g, float cosTheta) noexcept;

struct VolumetricFogSegment {
    Float3 inscattered{};
    float transmittance{1.0F};
    std::uint64_t shadowRays{};
};
// The fog along one ray (origin and length in world units; direction unit). `occluder` returns the
// nearest hit toward the sun (nullptr-like empty function: nothing casts shadows). `jitter` in
// [0,1) places each step's shadow sample.
[[nodiscard]] VolumetricFogSegment integrate_volumetric_fog(const RenderEnvironment& environment, Float3 origin, Float3 direction,
                                                            float length, float metersPerUnit, const AmbientOcclusionRay& occluder,
                                                            std::uint32_t steps, float jitter);

struct VolumetricFogStats {
    std::uint64_t pixels{};
    std::uint64_t samples{};
    std::uint64_t shadowRays{};
};
// Applies the fog to a drawn frame: every pixel keeps T of its light and gains the in-scattered
// light; light passes are scaled by T and the in-scattered light goes to the fog pass. Rows run
// on `threads` threads (0 = hardware concurrency); per-pixel jitter is hashed from the pixel and
// frameSeed, so the result does not depend on the thread count.
VolumetricFogStats apply_volumetric_fog(PolygonRenderTarget& target, const PolygonCamera& camera, const RenderEnvironment& environment,
                                        float metersPerUnit, const AmbientOcclusionRay& occluder, std::uint32_t frameSeed = 0U,
                                        std::uint32_t threads = 0U);

} // namespace dve::render
