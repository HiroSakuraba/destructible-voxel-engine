#pragma once
#include "dve/water_volume.hpp"
#include <span>
namespace dve {
struct UnderwaterLamp {
 Float3 position{},direction{0,1,0},color{1,.94F,.82F};
 float intensity{12.F}; // scene-linear irradiance at zero range
 float range{25.F};     // hard safety cutoff for CPU raster pass, metres
 float innerCos{.96F},outerCos{.83F};
};
struct WaterRaySegment {
 float startMeters{},lengthMeters{};
 bool cameraSubmerged{},hitsSurface{};
};
struct UnderwaterOpticsInput {
 Float3 objectRadiance{};      // pre-tonemap HDR scene-linear RGB
 Float3 ambientSurfaceLight{1,1,1}; // sunlit water-column radiance at z=surface
 Float3 lampIrradiance{};      // already attenuated from source to object
 float objectDepthMeters{};
 float eyeDepthMeters{};
 float viewPathMeters{};
 float rayDirectionZ{};       // camera-to-object unit ray, world Z
 bool objectSubmerged{true};
};
// The full medium transmittance is Beer–Lambert exp(-(a+b)*length).
// Surface solar attenuation uses independently measured/estimated Kd, not a+b.
[[nodiscard]] Float3 water_transmittance(const WaterVolumeSample&,float metres)noexcept;
[[nodiscard]] Float3 underwater_daylight(const WaterVolumeSample&,float depth)noexcept;
[[nodiscard]] Float3 evaluate_underwater_lamp(const UnderwaterLamp&,
    const WaterVolumeSample&,Float3 worldPoint)noexcept;
[[nodiscard]] Float3 shade_underwater_radiance(const WaterVolumeSample&,
    const UnderwaterOpticsInput&)noexcept;
// Deterministically find the underwater part of a finite camera ray. A crossing
// is bisected against the authoritative OceanField (not a second wave model).
[[nodiscard]] WaterRaySegment water_ray_segment(const OceanField&,
    Float3 eye,Float3 unitDirection,float maximumMetres,double time)noexcept;
// Unpolarized water-to-air Fresnel: returns 1 for total internal reflection.
[[nodiscard]] float water_to_air_fresnel(float cosineToNormal)noexcept;
}
