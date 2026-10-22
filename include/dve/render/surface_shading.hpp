#pragma once

// Surface shading for the CPU reference renderers: the GPU's microfacet model
// (shaders/common/pbr_lighting.hlsli) mirrored line by line, split into its diffuse and specular
// lobes, plus the ambient (sky) light with its specular reflection.
//
// Direct light (ShadeDirectLight): Cook-Torrance with GGX distribution, height-correlated Smith
// visibility and Fresnel-Schlick; dielectric F0 = 0.08 x specular (0.04 at the default 0.5),
// metals reflect their base colour; diffuse = base x (1 - metallic) / pi x (1 - F). Fresnel's
// grazing term fades out below F0 0.02 (UE's convention), so specular = 0 is exactly Lambert.
//
// Units. The GPU functions return radiance per unit light radiance. The CPU renderers treat sun
// intensity as the irradiance a surface facing the sun receives and draw albedo x irradiance, i.e.
// pi x radiance; they multiply these lobes by pi (kCpuRadianceScale). The GPU path (not yet run)
// draws pi times darker for the same sun intensity.
//
// Ambient light, from the sky/ground hemisphere (the CPU renderers' environment):
//   diffuse  = base x (1 - metallic) x (1 - Es) x E(normal)   (E already occluded: AO or traced)
//   specular = E(reflection) x (F0 x A + B) x specular occlusion
// where A, B is the environment BRDF (split-sum; integrated with the same D and V as the direct
// lobe, so the two agree) and Es = F0 x A + B its directional albedo: the light the specular lobe
// takes is not also scattered diffusely (energy conservation; a white surface under a uniform sky
// reflects no more than it receives). Specular occlusion is Lagarde's approximation from AO, as
// in environment_ibl.hlsli.

#include "dve/render_environment.hpp"
#include "dve/types.hpp"

namespace dve::render {

inline constexpr float kCpuRadianceScale = 3.14159265358979323846F;

// pbr_lighting.hlsli, function for function.
[[nodiscard]] float distribution_ggx(float NdotH, float roughness) noexcept;
[[nodiscard]] float visibility_smith_ggx_correlated(float NdotV, float NdotL, float roughness) noexcept;
[[nodiscard]] Float3 fresnel_schlick(float cosTheta, Float3 f0) noexcept;
[[nodiscard]] Float3 specular_f0(Float3 baseColor, float metallic, float specular) noexcept;

// A surface's light split into the passes a compositor works with; their sum is the image.
struct LightPasses {
    Float3 directDiffuse{};
    Float3 directSpecular{};
    Float3 indirectDiffuse{};
    Float3 indirectSpecular{};
    Float3 emission{};
    Float3 transmission{};   // light through Subsurface materials
    [[nodiscard]] Float3 sum() const noexcept {
        return {directDiffuse.x + directSpecular.x + indirectDiffuse.x + indirectSpecular.x + emission.x + transmission.x,
                directDiffuse.y + directSpecular.y + indirectDiffuse.y + indirectSpecular.y + emission.y + transmission.y,
                directDiffuse.z + directSpecular.z + indirectDiffuse.z + indirectSpecular.z + emission.z + transmission.z};
    }
};

struct ShadingLobes {
    Float3 diffuse{};
    Float3 specular{};
};

// ShadeDirectLight, as its two lobes (their sum is the HLSL result).
[[nodiscard]] ShadingLobes shade_direct_light(Float3 normal, Float3 viewDirection, Float3 lightDirection, Float3 lightRadiance,
                                              Float3 baseColor, float metallic, float roughness, float specular) noexcept;
// ShadeClearCoatDirect: the base lobes attenuated by the coat's Fresnel on the way in and out,
// the coat lobe added to specular.
[[nodiscard]] ShadingLobes shade_clear_coat_direct(Float3 normal, Float3 viewDirection, Float3 lightDirection,
                                                   Float3 lightRadiance, Float3 baseColor, float metallic, float roughness,
                                                   float specular, float clearCoat, float clearCoatRoughness) noexcept;

// The environment BRDF (A, B): the specular lobe's directional albedo is F0 x A + B. Tabulated
// once (32 x 32, NdotV x roughness) by integrating the direct lobe's D x V over the hemisphere.
struct EnvironmentBrdf {
    float scale{};
    float bias{};
};
[[nodiscard]] EnvironmentBrdf environment_brdf(float NdotV, float roughness) noexcept;
// Numerical directional albedo of the direct specular lobe for one F0 (the table's ground truth).
[[nodiscard]] Float3 specular_directional_albedo(float NdotV, float roughness, Float3 f0, std::uint32_t samples) noexcept;

// Lagarde's specular occlusion from AO (environment_ibl.hlsli).
[[nodiscard]] float specular_occlusion(float NdotV, float ambientOcclusion, float roughness) noexcept;

// Ambient light: `irradiance` is the sky/ground (or traced) light arriving around the normal,
// already occluded, and `reflected` the sky light arriving along the reflection direction, both
// in the CPU convention (what a white diffuse surface facing them would draw). The AO only
// occludes the specular reflection here.
[[nodiscard]] ShadingLobes shade_ambient_light(Float3 normal, Float3 viewDirection, Float3 irradiance, Float3 reflected,
                                               Float3 baseColor, float metallic, float roughness, float specular,
                                               float ambientOcclusion) noexcept;

} // namespace dve::render
