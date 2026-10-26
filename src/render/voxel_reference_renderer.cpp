#include "dve/voxel_metric.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/render/scene_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <functional>
#include <thread>
#include <vector>

#include "dve/polygon_bvh.hpp"
#include "dve/query.hpp"
#include "dve/render/caustic_map.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/ray_lighting_reference.hpp"
#include "dve/render/subsurface.hpp"
#include "dve/render/surface_shading.hpp"
#include "dve/render/atmosphere.hpp"
#include "dve/render/volumetric_fog.hpp"
#include "dve/voxel_normal_map.hpp"
#include "dve/voxel_textures.hpp"
#include "dve/voxel_displacement.hpp"
#include "dve/master_material.hpp"
#include "dve/material_layer_rules.hpp"
#include "dve/material_decal.hpp"
#include "dve/surface_marks.hpp"

namespace dve::render {
namespace {
constexpr float kEpsilon = 1.0e-7F;
constexpr float kPi = 3.14159265358979323846F;

Float3 cross3(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
Float3 multiply3(Float3 a, Float3 b) noexcept { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
Float3 add3(Float3 a, Float3 b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z}; }

struct CameraBasis {
    Float3 right{};
    Float3 up{};
    Float3 forward{};
    float tanHalf{};
    float aspect{};
};

CameraBasis make_basis(const PolygonCamera& camera, std::uint32_t width,
                       std::uint32_t height) noexcept {
    CameraBasis basis;
    basis.forward = normalize(subtract(camera.target, camera.position));
    basis.right = normalize(cross3(basis.forward, camera.up));
    if (length_squared(basis.right) < kEpsilon) basis.right = {1.0F, 0.0F, 0.0F};
    basis.up = normalize(cross3(basis.right, basis.forward));
    basis.tanHalf = std::tan(camera.verticalFieldOfViewRadians * 0.5F);
    basis.aspect = static_cast<float>(width) / static_cast<float>(height);
    return basis;
}

CameraBasis make_basis(const PolygonCamera& camera, const PolygonRenderTarget& target) noexcept {
    return make_basis(camera, target.width, target.height);
}

Float3 primary_ray(const CameraBasis& basis, std::uint32_t x, std::uint32_t y,
                   std::uint32_t width, std::uint32_t height) noexcept {
    const float ndcY = 1.0F - 2.0F *
        ((static_cast<float>(y) + 0.5F) / static_cast<float>(height));
    const float ndcX = 2.0F *
        ((static_cast<float>(x) + 0.5F) / static_cast<float>(width)) - 1.0F;
    Float3 ray = basis.forward;
    ray = add(ray, multiply(basis.right, ndcX * basis.tanHalf * basis.aspect));
    ray = add(ray, multiply(basis.up, ndcY * basis.tanHalf));
    return normalize(ray);
}

const VoxelMaterialDefinition& diagnostic_material() noexcept {
    static const VoxelMaterialDefinition material = [] {
        VoxelMaterialDefinition value;
        value.name = "Missing voxel material";
        value.baseColor = {1.0F, 0.0F, 1.0F, 1.0F};
        value.roughness = 1.0F;
        return value;
    }();
    return material;
}

const VoxelMaterialDefinition& material_for(const VoxelReferenceInstance& instance, MaterialId id,
                                             bool* fallback = nullptr) noexcept {
    const std::size_t index = static_cast<std::size_t>(id);
    if (index < instance.materials.size()) return instance.materials[index];
    if (fallback) *fallback = true;
    return diagnostic_material();
}

Float3 environment_radiance(const RenderEnvironment& environment, Float3 direction) noexcept {
    return spwi::environment_radiance(environment, direction);
}

float radical_inverse(std::uint32_t bits) noexcept {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

void make_basis(Float3 normal, Float3& tangent, Float3& bitangent) noexcept {
    const Float3 helper = std::abs(normal.z) < 0.999F ? Float3{0.0F, 0.0F, 1.0F}
                                                       : Float3{0.0F, 1.0F, 0.0F};
    tangent = normalize(cross3(helper, normal));
    bitangent = normalize(cross3(normal, tangent));
}

Float3 cosine_hemisphere(Float3 normal, std::uint32_t sample, std::uint32_t count,
                         std::uint32_t rotationSeed) noexcept {
    const float u = (static_cast<float>(sample) + 0.5F) / static_cast<float>(count);
    const float v = std::fmod(radical_inverse(sample ^ rotationSeed) +
                              radical_inverse(rotationSeed) * 0.5F, 1.0F);
    const float radius = std::sqrt(u);
    const float angle = 2.0F * kPi * v;
    const float x = radius * std::cos(angle);
    const float y = radius * std::sin(angle);
    const float z = std::sqrt(std::max(0.0F, 1.0F - u));
    Float3 tangent{}, bitangent{};
    make_basis(normal, tangent, bitangent);
    return normalize(add3(add3(multiply(tangent, x), multiply(bitangent, y)), multiply(normal, z)));
}

// PcgHash / Hash2 from random_sampling.hlsli.
std::uint32_t pcg_hash(std::uint32_t input) noexcept {
    const std::uint32_t state = input * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}
Float2 hash2(std::uint32_t seed) noexcept {
    const std::uint32_t a = pcg_hash(seed);
    const std::uint32_t b = pcg_hash(a);
    return {static_cast<float>(a) / 4294967295.0F, static_cast<float>(b) / 4294967295.0F};
}

Float3 sample_sun_disk(Float3 sunDirection, float angularRadius, std::uint32_t sample,
                       std::uint32_t count, std::uint32_t rotationSeed) noexcept {
    if (!(angularRadius > 0.0F)) return sunDirection;
    // Even spread over the sun disc (concentric map, as generate_shadow_rays.hlsl): the fraction
    // of rays that get through is the visible part of the disc, so overlapping penumbrae combine
    // as the union of what each occluder hides (the shadow blister).
    // Same pattern as the GPU: Hammersley points with a per-pixel Cranley-Patterson rotation of
    // both coordinates (RotatedHammersley2D with Hash2(seed)), so every seed shifts the whole set.
    const Float2 square = rotated_hammersley_2d(sample, count, hash2(rotationSeed));
    const Float2 disk = concentric_square_to_disk(square);
    const float scale = std::tan(angularRadius);
    Float3 tangent{}, bitangent{};
    make_basis(sunDirection, tangent, bitangent);
    return normalize(add3(sunDirection,
        add3(multiply(tangent, disk.x * scale), multiply(bitangent, disk.y * scale))));
}

// Secondary rays go through spwi::VoxelSceneTracer, whose hits are bit-identical to
// raycast_voxels_transformed over the same instances (tested in dve_rc_spwi_tests).
// Extra sun inputs: polygon occluders and caustic light.
struct SunExtras {
    struct Occluder {
        std::shared_ptr<const PolygonBvh> bvh;
        RigidTransform transform;
        const CookedPolygonAsset* asset{};
        float tintAlpha{1.0F};
        // Set when the asset has a Masked material: its textures, for the alpha test.
        std::shared_ptr<const std::vector<PolygonTextureMipChain>> maskedMips;
    };
    std::vector<Occluder> occluders;
    // Polygons that ambient-occlusion rays see (sunOccluders and occlusionPolygons).
    std::vector<Occluder> aoOccluders;
    const CausticMap* caustics{};
    // Damage marks and decals on the voxel surfaces (ReferenceVoxelRenderer::surfaceMarks, decals).
    std::span<const SurfaceMark> surfaceMarks{};
    std::span<const MaterialDecal> decals{};
    [[nodiscard]] bool polygon_occluded(Float3 origin, Float3 direction, float maximumDistance) const {
        return nearest(occluders, origin, direction, maximumDistance, true).has_value();
    }
    [[nodiscard]] std::optional<float> ao_nearest(Float3 origin, Float3 direction, float maximumDistance) const {
        return nearest(aoOccluders, origin, direction, maximumDistance, false);
    }
    // Nearest polygon hit (world distance along the unit direction); `any` stops at the first.
    [[nodiscard]] static std::optional<float> nearest(const std::vector<Occluder>& list, Float3 origin, Float3 direction,
                                                      float maximumDistance, bool any) {
        std::optional<float> best;
        for (const Occluder& o : list) {
            Float3 localOrigin = inverse_transform_point(o.transform, origin);
            const Float3 localDirection = normalize(inverse_transform_vector(o.transform, direction));
            float remaining = best ? *best : maximumDistance;
            float travelled = 0.0F;
            // Masked surfaces block only where they pass the alpha test, as the rasterizer draws
            // them; through their holes the ray goes on.
            for (int crossing = 0; crossing < 64; ++crossing) {
                const auto hit = o.bvh->raycast(localOrigin, localDirection, remaining);
                if (!hit) break;
                if (o.maskedMips && hit->materialIndex < o.asset->materials.size() &&
                    o.asset->materials[hit->materialIndex].blendMode == MaterialBlendMode::Masked &&
                    hit->materialIndex < o.asset->materialBindings.size()) {
                    const float opacity = polygon_hit_opacity(
                        *o.asset, *hit, transform_point(o.transform, hit->position),
                        normalize(transform_vector(o.transform, hit->normal)), *o.maskedMips, o.tintAlpha);
                    if (opacity < o.asset->materialBindings[hit->materialIndex].alphaCutoff) {
                        const float step = hit->distance + 1.0e-4F;
                        localOrigin = add(localOrigin, multiply(localDirection, step));
                        remaining -= step;
                        travelled += step;
                        if (!(remaining > 0.0F)) break;
                        continue;
                    }
                }
                best = travelled + hit->distance;
                if (any) return best;
                break;
            }
        }
        return best;
    }
};

float hard_visibility(const spwi::VoxelSceneTracer& tracer, Float3 origin,
                      Float3 direction, float maximumDistance, VoxelReferenceRenderStats& stats,
                      const SunExtras* extras = nullptr) {
    ++stats.shadowRays;
    if (tracer.occluded(origin, direction, maximumDistance) ||
        (extras && extras->polygon_occluded(origin, direction, maximumDistance))) {
        ++stats.shadowBlockedRays;
        return 0.0F;
    }
    return 1.0F;
}

float shadow_visibility(const spwi::VoxelSceneTracer& tracer, Float3 point,
                        Float3 normal, const RenderEnvironment& environment,
                        const VoxelLightingDistances& distances, std::uint32_t pixelSeed, VoxelReferenceRenderStats& stats,
                        const SunExtras* extras = nullptr) {
    if (environment.shadowMode == ShadowMode::Off) return 1.0F;
    const Float3 sun = normalize(environment.sunDirection);
    const Float3 origin = add(point, multiply(normal, distances.shadowBias));
    auto weighted = [&](float raw) {
        return std::clamp(1.0F - environment.shadowStrength * (1.0F - raw), 0.0F, 1.0F);
    };
    if (environment.shadowMode == ShadowMode::Hard)
        return weighted(hard_visibility(tracer, origin, sun,
                                        distances.shadowMaxDistance, stats, extras));
    if (environment.shadowMode == ShadowMode::Contact)
        return weighted(hard_visibility(tracer, origin, sun,
                                        distances.contactShadowDistance, stats, extras));

    const std::uint32_t samples = std::clamp(environment.shadowSamples, 1U, 16U);
    float visible = 0.0F;
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
        const Float3 direction = sample_sun_disk(sun, environment.shadowSoftnessRadians,
                                                 sample, samples, pixelSeed);
        visible += hard_visibility(tracer, origin, direction,
                                   distances.shadowMaxDistance, stats, extras);
    }
    visible /= static_cast<float>(samples);
    if (environment.shadowMode == ShadowMode::Hybrid) {
        const float contact = hard_visibility(tracer, origin, sun,
                                              distances.contactShadowDistance, stats, extras);
        visible = (std::min)(visible, contact);
    }
    return weighted(visible);
}

// Sunlight scattered through a Subsurface material to its shaded side (see subsurface.hpp):
// march toward the sun through the same material to where the light enters, then that point's
// own sun visibility (the far side can be shadowed too) and the Burley transmittance.
Float3 subsurface_light(const spwi::VoxelSceneTracer& tracer, const VoxelReferenceInstance& instance, MaterialId id,
                        const VoxelMaterialDefinition& material, Float3 point, Float3 normal,
                        const RenderEnvironment& environment, const VoxelLightingDistances& distances,
                        std::uint32_t pixelSeed, VoxelReferenceRenderStats& stats, const SunExtras* extras) {
    if (!material_has_subsurface(material) || !(distances.subsurfaceMaxDistance > 0.0F)) return {};
    const Float3 sun = normalize(environment.sunDirection);
    if (!(dot(normal, sun) < 0.0F)) return {};   // sunlit side: the ordinary diffuse term
    ++stats.subsurfaceMarches;
    const Float3 start = add(point, multiply(normal, -1.0e-3F));
    const SubsurfaceExit exit = march_voxel_subsurface(instance, id, start, sun, distances.subsurfaceMaxDistance);
    if (!exit.found || !exit.open) return {};
    ++stats.subsurfaceExits;
    const float visibility = shadow_visibility(tracer, exit.position, exit.normal, environment, distances,
                                               pixelSeed ^ 0x9E3779B9U, stats, extras);
    return subsurface_transmitted_radiance(material, exit, sun, distances.metersPerVoxel, visibility, environment);
}

// The sun's share at a point: cos(normal, sun) x visibility.
float sun_term(Float3 normal, const RenderEnvironment& environment, float visibility) noexcept {
    return (std::max)(0.0F, dot(normal, normalize(environment.sunDirection))) * visibility;
}

// Front-lit subsurface diffusion: the sun term averaged over the surface around the point with
// Burley's profile (subsurface.hpp), so light spreads under the surface into shadow edges and
// past the terminator. Sample radii follow the profile exactly (its two exponentials, chosen with
// weights 1/4 and 3/4), so the estimate is a plain average. Each disc point is projected onto the
// surface along the normal (a probe ray onto the same instance and material); points with no such
// surface there are left out and the rest renormalised. One sun ray per sample, across the sun
// disc for soft shadows.
float diffused_sun_term(const spwi::VoxelSceneTracer& tracer, std::size_t instanceIndex, MaterialId id,
                        const VoxelMaterialDefinition& material, Float3 point, Float3 normal,
                        const RenderEnvironment& environment, const VoxelLightingDistances& distances,
                        std::uint32_t samples, std::uint32_t pixelSeed, VoxelReferenceRenderStats& stats,
                        const SunExtras* extras) {
    const float d = material.subsurfaceScatterDistanceMeters / distances.metersPerVoxel;
    const Float3 sun = normalize(environment.sunDirection);
    Float3 tangent{}, bitangent{};
    make_basis(normal, tangent, bitangent);
    const bool shadows = environment.shadowMode != ShadowMode::Off;
    const float reach = environment.shadowMode == ShadowMode::Contact ? distances.contactShadowDistance
                                                                       : distances.shadowMaxDistance;
    float sum = 0.0F;
    std::uint32_t valid = 0;
    for (std::uint32_t i = 0; i < samples; ++i) {
        const float u = (static_cast<float>(i) + 0.5F) / static_cast<float>(samples);
        const float v = std::fmod(radical_inverse(i ^ pixelSeed) + radical_inverse(pixelSeed), 1.0F);
        const float r = burley_sample_radius(u, d);
        const float angle = 2.0F * kPi * v;
        Float3 q = point, qn = normal;
        if (r > 1.0e-3F) {
            const Float3 offset = add3(multiply(tangent, r * std::cos(angle)), multiply(bitangent, r * std::sin(angle)));
            const float h = (std::max)(r, 1.0F);
            ++stats.subsurfaceDiffusionProbes;
            const auto hit = tracer.trace_instance(instanceIndex, add3(add3(point, offset), multiply(normal, h)),
                                                   multiply(normal, -1.0F), 2.0F * h);
            if (!hit || hit->objectHit.material != id) continue;
            q = hit->worldPosition;
            qn = hit->worldNormal;
        }
        ++valid;
        const float cosine = (std::max)(0.0F, dot(qn, sun));
        if (!(cosine > 0.0F)) continue;
        float visibility = 1.0F;
        if (shadows) {
            const Float3 direction = environment.shadowMode == ShadowMode::Soft || environment.shadowMode == ShadowMode::Hybrid
                ? sample_sun_disk(sun, environment.shadowSoftnessRadians, i, samples, pixelSeed) : sun;
            const float raw = hard_visibility(tracer, add(q, multiply(qn, distances.shadowBias)), direction, reach, stats, extras);
            visibility = std::clamp(1.0F - environment.shadowStrength * (1.0F - raw), 0.0F, 1.0F);
        }
        sum += cosine * visibility;
    }
    return valid > 0U ? sum / static_cast<float>(valid) : 0.0F;
}

// Ambient occlusion is applied where nothing traced already sees occluders: the
// AmbientHemisphere ambient (traced one-bounce GI and radiance cascades see them through their
// own rays).
bool ambient_occlusion_active(const RenderEnvironment& environment, const VoxelLightingDistances& distances) noexcept {
    return environment.globalIlluminationMode == GlobalIlluminationMode::AmbientHemisphere &&
           environment.ambientOcclusionSamples > 0U && distances.ambientOcclusionMaxDistance > 0.0F;
}
// The specular reflection of the sky is occluded by AO in every ambient mode (traced GI traces
// diffuse light only).
bool specular_occlusion_active(const RenderEnvironment& environment, const VoxelLightingDistances& distances) noexcept {
    return environment.globalIlluminationMode != GlobalIlluminationMode::Off &&
           environment.ambientOcclusionSamples > 0U && distances.ambientOcclusionMaxDistance > 0.0F;
}

// reference_ambient_occlusion (ray_lighting_reference.hpp, the GPU passes' mirror) against the
// voxels and the AO polygons.
float voxel_ambient_occlusion(const spwi::VoxelSceneTracer& tracer, const SunExtras* extras, Float3 point, Float3 normal,
                              std::uint32_t pixelIndex, const RenderEnvironment& environment,
                              const VoxelLightingDistances& distances, VoxelReferenceRenderStats& stats) {
    return reference_ambient_occlusion(point, normal, pixelIndex, 0U, environment, distances.metersPerVoxel,
        [&](Float3 origin, Float3 direction, float maximum) -> std::optional<float> {
            ++stats.ambientOcclusionRays;
            std::optional<float> best;
            if (const auto hit = tracer.trace(origin, direction, maximum)) best = hit->objectHit.distance;
            if (extras)
                if (const auto polygon = extras->ao_nearest(origin, direction, best ? *best : maximum))
                    if (!best || *polygon < *best) best = polygon;
            return best;
        });
}

Float3 global_illumination(const spwi::VoxelSceneTracer& tracer, Float3 point,
                           Float3 normal, const RenderEnvironment& environment,
                           const VoxelLightingDistances& distances, std::uint32_t pixelSeed, VoxelReferenceRenderStats& stats,
                           std::uint32_t pixelIndex = 0U, const SunExtras* extras = nullptr, float* aoOut = nullptr) {
    if (environment.globalIlluminationMode == GlobalIlluminationMode::Off) return {};
    const float ao = specular_occlusion_active(environment, distances)
        ? voxel_ambient_occlusion(tracer, extras, point, normal, pixelIndex, environment, distances, stats) : 1.0F;
    if (aoOut) *aoOut = ao;
    if (environment.globalIlluminationMode == GlobalIlluminationMode::AmbientHemisphere)
        return multiply(sky_irradiance(environment, normal), environment.globalIlluminationIntensity * ao);

    const std::uint32_t samples = std::clamp(environment.globalIlluminationSamples, 1U, 16U);
    const Float3 origin = add(point, multiply(normal, distances.shadowBias));
    Float3 accumulated{};
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
        const Float3 direction = cosine_hemisphere(normal, sample, samples, pixelSeed);
        ++stats.globalIlluminationRays;
        const auto hit = tracer.trace(origin, direction,
                                      distances.globalIlluminationMaxDistance);
        if (!hit) {
            accumulated = add3(accumulated, environment_radiance(environment, direction));
            continue;
        }
        ++stats.globalIlluminationHits;
        // Same bounce model as resolve_gi.hlsl: hemisphere ambient plus sun with an explicit
        // visibility ray (one_bounce_diffuse_radiance). Previously only the ambient term was used.
        accumulated = add3(accumulated, spwi::bounce_hit_radiance(
            tracer, *hit, environment, &stats.globalIlluminationSunRays,
            distances.metersPerVoxel));
    }
    return multiply(accumulated,
                    environment.globalIlluminationIntensity / static_cast<float>(samples));
}

// Everything one voxel surface point needs for shading, in the CPU convention (light values are
// what a white diffuse surface facing them would draw).
struct VoxelSurfaceInput {
    Float3 normal{};
    Float3 viewDirection{};       // toward the camera
    float visibility{1.0F};       // the point's own sun visibility (shadow strength applied)
    float sunTerm{};              // cos(normal, sun) x visibility, or its subsurface-diffused average
    Float3 ambient{};             // ambient light arriving around the normal, already occluded
    float ambientOcclusion{1.0F}; // for the specular reflection
    Float3 caustic{};             // caustic irradiance, x full sun
    Float3 transmitted{};         // subsurface translucency
};

// The shading split into its light passes (surface_shading.hpp): direct diffuse and specular from
// the sun (caustic light is direct sunlight that came through glass, diffuse only), ambient
// diffuse and its specular reflection of the sky, emission, and light transmitted through
// Subsurface materials. Their sum is the image.
LightPasses shade_voxel(const VoxelMaterialDefinition& material, const RenderEnvironment& environment,
                        const VoxelSurfaceInput& in) noexcept {
    LightPasses out;
    const Float3 base{material.baseColor.x, material.baseColor.y, material.baseColor.z};
    if (material.shadingModel == MaterialShadingModel::Unlit) { out.emission = base; return out; }
    if (material.shadingModel == MaterialShadingModel::Emissive) { out.emission = add3(base, material.emissive); return out; }
    const Float3 sun = normalize(environment.sunDirection);
    const Float3 sunIrradiance = sun_irradiance(environment);
    const Float3 v = in.viewDirection;
    // Diffuse: base x (1 - metallic) x (1 - F) x sun x sunTerm (Lambert, as shade_direct_light's
    // diffuse lobe x pi, but with the subsurface-diffused sun term where there is one).
    const Float3 f0 = specular_f0(base, material.metallic, material.specular);
    const Float3 half = normalize(add(v, sun));
    const Float3 fresnel = fresnel_schlick(std::clamp(dot(v, half), 0.0F, 1.0F), f0);
    const Float3 diffuseColor = multiply3(multiply(base, 1.0F - material.metallic),
                                          Float3{1.0F - fresnel.x, 1.0F - fresnel.y, 1.0F - fresnel.z});
    out.directDiffuse = multiply(multiply3(diffuseColor, sunIrradiance), in.sunTerm);
    // Caustic irradiance is relative to full sun head on (it carries its own cosine). It is the
    // light glass and water let through in place of what their shadow removed, so it follows the
    // shadow: shadowStrength of it, none with shadows off (all the direct sun is already there).
    const float causticShare = environment.shadowMode == ShadowMode::Off ? 0.0F
                                                                         : std::clamp(environment.shadowStrength, 0.0F, 1.0F);
    out.directDiffuse = add3(out.directDiffuse,
                             multiply(multiply3(base, multiply3(sunIrradiance, in.caustic)), causticShare * (1.0F - material.metallic)));
    // Specular from the sun at this point (its own visibility).
    const ShadingLobes lobes = material.shadingModel == MaterialShadingModel::ClearCoat
        ? shade_clear_coat_direct(in.normal, v, sun, sunIrradiance, base, material.metallic, material.roughness,
                                  material.specular, material.clearCoat, material.clearCoatRoughness, material.iridescence,
                                  material.iridescenceIor, material.iridescenceThicknessNm, material.indexOfRefraction)
        : shade_direct_light(in.normal, v, sun, sunIrradiance, base, material.metallic, material.roughness,
                             material.specular, material.iridescence, material.iridescenceIor,
                             material.iridescenceThicknessNm, material.indexOfRefraction);
    Float3 specularSum = lobes.specular;
    if (material_has_sheen(material))
        specularSum = add3(specularSum, sheen_direct(in.normal, v, sun, sunIrradiance, material.sheenColor,
                                                     material.sheenRoughness));
    out.directSpecular = multiply(specularSum, kCpuRadianceScale * in.visibility);
    if (material.shadingModel == MaterialShadingModel::ClearCoat && material.clearCoat > 0.0F) {
        // The coat's Fresnel dims the base lobe on the way in and again on the way out.
        const Float3 coat = fresnel_schlick(std::clamp(dot(v, half), 0.0F, 1.0F), {0.04F, 0.04F, 0.04F});
        const float c = std::clamp(material.clearCoat, 0.0F, 1.0F);
        const Float3 through{1.0F - c * coat.x, 1.0F - c * coat.y, 1.0F - c * coat.z};
        out.directDiffuse = multiply3(out.directDiffuse, multiply3(through, through));
    }
    // Ambient: diffuse, and the sky reflected along the mirror direction.
    if (environment.globalIlluminationMode != GlobalIlluminationMode::Off) {
        const Float3 reflection = subtract(multiply(in.normal, 2.0F * dot(in.normal, v)), v);
        const Float3 reflected = multiply(environment_radiance(environment, reflection), environment.globalIlluminationIntensity);
        const ShadingLobes ambient = shade_ambient_light(in.normal, v, in.ambient, reflected, base, material.metallic,
                                                         material.roughness, material.specular, in.ambientOcclusion,
                                                         material.iridescence, material.iridescenceIor,
                                                         material.iridescenceThicknessNm, material.indexOfRefraction);
        out.indirectDiffuse = ambient.diffuse;
        out.indirectSpecular = ambient.specular;
    }
    out.transmission = in.transmitted;   // carries the subsurface colour as its albedo
    out.emission = material.emissive;
    return out;
}


void apply_scene_lighting(LightPasses &light, const SceneLighting *scene,
                          Float3 point, const VoxelMaterialDefinition &material,
                          const VoxelSurfaceInput &surface,
                          const RenderEnvironment &env, std::uint32_t seed) {
  if (!scene)
    return;
  const auto local = scene->direct(point, surface.normal, surface.viewDirection,
                                   material, seed);
  light.directDiffuse = add3(light.directDiffuse, local.diffuse);
  light.directSpecular = add3(light.directSpecular, local.specular);
  if (env.pathTraceBounces > 0 &&
      env.globalIlluminationMode != GlobalIlluminationMode::Off) {
    const auto bounce = scene->indirect_lobes(
        point, surface.normal, surface.viewDirection, material, seed);
    light.indirectDiffuse = bounce.diffuse;
    light.indirectSpecular = bounce.specular;
  }
}


// Everything a voxel material's images do to one shaded point, in order: parallax (displacement
// drawn as shading: a Parallax material, or a Traced one beyond the tracer's focus) moves where
// the images are read and tilts the normal; the normal map bends it further; the texture set
// colours the shading copy of the material and its occlusion dims the ambient light. Traced
// displacement already shaped the hit (position and normal) in the tracer.
// Returns where the maps were read (object space, metres): the hit, or the parallax point.
Float3 apply_voxel_surface_maps(const spwi::VoxelSceneTracer& tracer, const VoxelReferenceInstance& instance,
                                VoxelMaterialDefinition& material, const RenderEnvironment& environment, Float3 worldPosition,
                                Float3 faceNormalWorld, bool diffused, float metersPerVoxel, float footprintMeters,
                                VoxelSurfaceInput& surface) noexcept {
    const float units = resolve_meters_per_voxel(metersPerVoxel);
    Float3 meters = multiply(inverse_transform_point(instance.transform, worldPosition), units);
    const bool images = material_has_normal_map(material) || material_has_textures(material) || material_has_displacement(material);
    if (!images) return meters;
    // The hit's axis-aligned face in object space (for projections and parallax).
    Float3 face = inverse_transform_vector(instance.transform, faceNormalWorld);
    const float ax = std::abs(face.x), ay = std::abs(face.y), az = std::abs(face.z);
    face = ax >= ay && ax >= az ? Float3{face.x > 0 ? 1.0F : -1.0F, 0, 0}
         : ay >= az            ? Float3{0, face.y > 0 ? 1.0F : -1.0F, 0}
                               : Float3{0, 0, face.z > 0 ? 1.0F : -1.0F};
    bool changed = false;
    if (material_has_displacement(material) && (material.displacementMode == 1U || !tracer.displacement_in_focus(worldPosition))) {
        const float level = material.heightMap
            ? std::max(0.0F, std::log2(std::max(footprintMeters, 1.0e-9F) * static_cast<float>(std::max(material.heightMap->width, material.heightMap->height)) /
                                       std::max(material.textureTileMeters, 1.0e-4F)))
            : 0.0F;
        const VoxelParallaxResult parallax = voxel_parallax(material, meters, face, normalize(inverse_transform_vector(instance.transform,
                                                                                                                         surface.viewDirection)),
                                                            voxel_displacement_depth(material, units), level);
        meters = parallax.positionMeters;
        surface.normal = normalize(transform_vector(instance.transform, parallax.normal));
        changed = true;
    }
    if (material_has_normal_map(material)) {
        const Float3 localNormal = normalize(inverse_transform_vector(instance.transform, surface.normal));
        surface.normal = normalize(transform_vector(instance.transform, voxel_normal_map_normal(material, meters, localNormal)));
        changed = true;
    }
    if (material_has_textures(material)) {
        const VoxelTextureSample maps = sample_voxel_textures(material, meters, face, footprintMeters);
        apply_voxel_texture_sample(material, maps);
        surface.ambient = multiply(surface.ambient, maps.occlusion);
        surface.ambientOcclusion *= maps.occlusion;
    }
    if (changed && !diffused) surface.sunTerm = sun_term(surface.normal, environment, surface.visibility);
    return meters;
}

// Material layers on voxels: each enabled layer blends its source material over the shaded one
// by weight x the layer's rules (dve/material_layer_rules.hpp) at this point. A source's own
// layers are not applied. Returns how many layers covered the point.
std::uint32_t apply_voxel_material_layers(const VoxelReferenceInstance& instance, VoxelMaterialDefinition& material,
                                          Float3 worldPosition, Float3 worldNormal, float ambientOcclusion, Float3 mapMeters,
                                          float metersPerVoxel, float footprintMeters) noexcept {
    if (material.layers.empty()) return 0U;
    const std::size_t count = std::min<std::size_t>(material.layers.size(), kMaximumVoxelMaterialLayers);
    bool rules = false;
    for (std::size_t i = 0; i < count; ++i) rules = rules || material.layers[i].rules.enabled;
    const float units = resolve_meters_per_voxel(metersPerVoxel);
    MaterialLayerRuleContext context;
    context.worldPositionMeters = multiply(worldPosition, units);
    context.worldNormal = worldNormal;
    context.ambientOcclusion = ambientOcclusion;
    context.footprintMeters = footprintMeters;
    if (rules) {
        // The solid cell under the point, its exposed faces and the distance to their edges.
        const Float3 localWorld = inverse_transform_point(instance.transform, worldPosition);
        const Float3 localNormal = inverse_transform_vector(instance.transform, worldNormal);
        const Float3 spacing = instance.spacing;
        const Float3 cellPoint{localWorld.x / spacing.x - localNormal.x * 1.0e-3F, localWorld.y / spacing.y - localNormal.y * 1.0e-3F,
                               localWorld.z / spacing.z - localNormal.z * 1.0e-3F};
        const Int3 cell{static_cast<int>(std::floor(cellPoint.x)), static_cast<int>(std::floor(cellPoint.y)),
                        static_cast<int>(std::floor(cellPoint.z))};
        std::uint8_t exposed = 0U;
        if (instance.object) {
            const Int3 neighbours[6] = {{cell.x - 1, cell.y, cell.z}, {cell.x + 1, cell.y, cell.z}, {cell.x, cell.y - 1, cell.z},
                                        {cell.x, cell.y + 1, cell.z}, {cell.x, cell.y, cell.z - 1}, {cell.x, cell.y, cell.z + 1}};
            for (int k = 0; k < 6; ++k)
                if (!instance.object->occupied_at(neighbours[k])) exposed = static_cast<std::uint8_t>(exposed | (1U << k));
        }
        const float cellMeters = std::min({spacing.x, spacing.y, spacing.z}) * units;
        context.edgeDistanceMeters =
            voxel_exposed_edge_distance({cellPoint.x - static_cast<float>(cell.x), cellPoint.y - static_cast<float>(cell.y),
                                         cellPoint.z - static_cast<float>(cell.z)},
                                        exposed) * cellMeters;
        // The base material's height there (height map, else surface pattern).
        const float ax = std::abs(localNormal.x), ay = std::abs(localNormal.y), az = std::abs(localNormal.z);
        const Float3 face = ax >= ay && ax >= az ? Float3{localNormal.x > 0 ? 1.0F : -1.0F, 0, 0}
                          : ay >= az            ? Float3{0, localNormal.y > 0 ? 1.0F : -1.0F, 0}
                                                : Float3{0, 0, localNormal.z > 0 ? 1.0F : -1.0F};
        context.baseHeight = material.heightMap ? sample_voxel_height(material, mapMeters, face, 0.0F)
                           : surface_pattern_is_height_source(material.surfaceDetail)
                               ? surface_pattern_height(material.surfaceDetail, mapMeters, footprintMeters)
                               : 0.5F;
    }
    std::uint32_t applied = 0U;
    const VoxelMaterialDefinition base = material;
    for (std::size_t i = 0; i < count; ++i) {
        const VoxelMaterialLayer& layer = base.layers[i];
        if (!layer.enabled || !(layer.weight > 0.0F) || layer.sourceMaterial >= instance.materials.size()) continue;
        const float coverage = std::clamp(layer.weight, 0.0F, 1.0F) * material_layer_rule_factor(layer.rules, context);
        if (!(coverage > 0.0F)) continue;
        material = blend_material_layer(material, instance.materials[layer.sourceMaterial], coverage, layer.blendMode);
        ++applied;
    }
    return applied;
}

// Damage marks of this object, then decals, on a shaded voxel point. Returns whether the normal
// changed (decals can carry normals).
bool apply_voxel_marks_and_decals(const VoxelReferenceInstance& instance, const SunExtras& extras, VoxelMaterialDefinition& material,
                                  Float3 worldPosition, Float3 mapMeters, float footprintMeters, Float3& normal,
                                  VoxelReferenceRenderStats& stats) noexcept {
    if (!extras.surfaceMarks.empty()) {
        const SurfaceMarkSample marks = sample_surface_marks(extras.surfaceMarks, instance.objectId, mapMeters, footprintMeters);
        if (marks.any()) { apply_surface_marks(material, marks); ++stats.markedSurfaces; }
    }
    if (extras.decals.empty()) return false;
    MaterialSurfaceSample surface;
    surface.baseColor = material.baseColor;
    surface.normal = normal;
    surface.metallic = material.metallic;
    surface.roughness = material.roughness;
    surface.emissive = material.emissive;
    surface.opacity = material.baseColor.w;
    MaterialDecalCompositeStats decalStats;
    const MaterialSurfaceSample out = composite_material_decals(surface, worldPosition, normal, extras.decals, &decalStats);
    if (decalStats.applied == 0U) return false;
    ++stats.decalSurfaces;
    material.baseColor = out.baseColor;
    material.baseColor.w = out.opacity;
    material.metallic = out.metallic;
    material.roughness = out.roughness;
    material.emissive = out.emissive;
    const bool turned = out.normal.x != normal.x || out.normal.y != normal.y || out.normal.z != normal.z;
    normal = out.normal;
    return turned;
}

// Writes the pixel's render passes (polygon_renderer.hpp) when the target has them. The shadow pass
// is the direct light the point would get with its own sun visibility 1, less what it gets (for
// subsurface-diffused sun the point's own share is unshadowed, its neighbours' are not).
void write_voxel_passes(PolygonRenderTarget& target, std::size_t index, const VoxelMaterialDefinition& material,
                        const RenderEnvironment& environment, const VoxelSurfaceInput& surface, const LightPasses& light,
                        float cameraZ, float metersPerVoxel) {
    if (!target.passes.enabled) return;
    RenderPassSample sample{light, {}, surface.visibility, surface.normal, cameraZ * resolve_meters_per_voxel(metersPerVoxel)};
    if (surface.visibility < 1.0F) {
        VoxelSurfaceInput open = surface;
        const float cosine = sun_term(surface.normal, environment, 1.0F);
        open.sunTerm = surface.sunTerm + cosine - sun_term(surface.normal, environment, surface.visibility);
        open.visibility = 1.0F;
        const LightPasses lit = shade_voxel(material, environment, open);
        const auto own = shade_voxel(material, environment, surface);
        const Float3 removed = subtract(add3(lit.directDiffuse, lit.directSpecular), add3(own.directDiffuse, own.directSpecular));
        sample.shadow = {(std::max)(0.0F, removed.x), (std::max)(0.0F, removed.y), (std::max)(0.0F, removed.z)};
    }
    target.write_passes(index, sample, 1.0F, true);
}

Float4 beauty(const LightPasses& light, float alpha) noexcept {
    const Float3 sum = light.sum();
    return {sum.x, sum.y, sum.z, alpha};
}
} // namespace

namespace spwi {
VoxelGBuffer build_voxel_gbuffer(const VoxelSceneTracer& tracer, const PolygonCamera& camera,
                                 std::uint32_t width, std::uint32_t height,
                                 std::span<const float> initialDepth, std::uint32_t threadCount) {
    VoxelGBuffer gbuffer;
    gbuffer.width = width;
    gbuffer.height = height;
    gbuffer.cameraPosition = camera.position;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    gbuffer.texels.assign(count, VoxelGBufferTexel{});
    if (count == 0U || !(camera.nearPlane > 0.0F) || !(camera.farPlane > camera.nearPlane))
        return gbuffer;
    const CameraBasis basis = make_basis(camera, width, height);
    gbuffer.pixelWorldScale = 2.0F * basis.tanHalf / static_cast<float>(height);
    const float maximumDistance = camera.farPlane * 1.5F;
    const auto instances = tracer.instances();
    // Per pixel, instances are visited in submission order with the renderer's strict depth
    // test, which is the same result as the renderer's instance-outer loop.
    auto rows = [&](std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t y = begin; y < end; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                float depth = index < initialDepth.size() ? initialDepth[index] : 1.0F;
                const Float3 ray = primary_ray(basis, x, y, width, height);
                VoxelGBufferTexel& texel = gbuffer.texels[index];
                for (std::size_t k = 0; k < instances.size(); ++k) {
                    if (!instances[k].visible || instances[k].object == nullptr) continue;
                    const auto hit = tracer.trace_instance(k, camera.position, ray, maximumDistance);
                    if (!hit) continue;
                    const float cameraZ = dot(subtract(hit->worldPosition, camera.position), basis.forward);
                    if (!(cameraZ > camera.nearPlane && cameraZ < camera.farPlane)) continue;
                    const float normalizedDepth =
                        (cameraZ - camera.nearPlane) / (camera.farPlane - camera.nearPlane);
                    if (!(normalizedDepth < depth)) continue;
                    depth = normalizedDepth;
                    texel.valid = true;
                    texel.instance = static_cast<std::uint32_t>(k);
                    texel.material = hit->objectHit.material;
                    texel.position = hit->worldPosition;
                    texel.normal = hit->worldNormal;
                    texel.viewDistance = length(subtract(hit->worldPosition, camera.position));
                    texel.normalizedDepth = normalizedDepth;
                }
            }
        }
    };
    std::uint32_t threads = threadCount == 0U ? (std::max)(1U, std::thread::hardware_concurrency())
                                              : threadCount;
    threads = std::clamp(threads, 1U, (std::min)(16U, height));
    if (threads == 1U) {
        rows(0U, height);
    } else {
        std::vector<std::thread> workers;
        for (std::uint32_t t = 0; t < threads; ++t)
            workers.emplace_back(rows, height * t / threads, height * (t + 1U) / threads);
        for (std::thread& worker : workers) worker.join();
    }
    return gbuffer;
}
} // namespace spwi

namespace {
VoxelReferenceRenderStats render_radiance_cascades(
    const RadianceCascadeSettings& settings, const spwi::VoxelSceneTracer& tracer,
    std::span<const VoxelReferenceInstance> instances, const PolygonCamera& camera,
    const RenderEnvironment& environment, const VoxelLightingDistances& distances,
    PolygonRenderTarget& target, const SunExtras& extras, std::uint32_t diffusionSamples) {
    VoxelReferenceRenderStats stats;
    const auto diffuses = [&](const VoxelMaterialDefinition& m, const VoxelLightingDistances& d) {
        return diffusionSamples > 0U && material_has_subsurface(m) && d.subsurfaceMaxDistance > 0.0F;
    };
    for (const VoxelReferenceInstance& instance : instances) {
        ++stats.submittedInstances;
        if (instance.visible && instance.object != nullptr)
            stats.tracedRays += static_cast<std::uint64_t>(target.width) * target.height;
    }
    const spwi::VoxelGBuffer gbuffer = spwi::build_voxel_gbuffer(
        tracer, camera, target.width, target.height, target.depth, settings.threadCount);
    const spwi::IndirectResult gi =
        spwi::solve_radiance_cascades(tracer, gbuffer, environment, settings,
                                      distances.metersPerVoxel);
    stats.radianceCascadeIntervalRays = gi.stats.intervalRays;
    stats.globalIlluminationSunRays = gi.stats.sunRays;
    for (std::size_t index = 0; index < gbuffer.texels.size(); ++index) {
        const spwi::VoxelGBufferTexel& texel = gbuffer.texels[index];
        if (!texel.valid) continue;
        const VoxelReferenceInstance& instance = instances[texel.instance];
        bool fallback = false;
        VoxelMaterialDefinition material = material_for(instance, texel.material, &fallback);
        if (fallback) ++stats.materialFallbacks;
        const std::uint32_t seed = static_cast<std::uint32_t>(index) * 747796405U + 2891336453U;
        const float visibility = shadow_visibility(tracer, texel.position, texel.normal,
                                                   environment, distances, seed, stats, &extras);
        const Float3 caustic = extras.caustics ? extras.caustics->irradiance(texel.position, texel.normal,
                                                                     instance.objectId != 0U ? instance.objectId : instance.object->id())
                                               : Float3{};
        const Float3 transmitted = subsurface_light(tracer, instance, texel.material, material, texel.position,
                                                    texel.normal, environment, distances, seed, stats, &extras);
        const float sunShare = diffuses(material, distances)
            ? diffused_sun_term(tracer, texel.instance, texel.material, material, texel.position, texel.normal, environment,
                                distances, diffusionSamples, seed, stats, &extras)
            : sun_term(texel.normal, environment, visibility);
        const float ao = specular_occlusion_active(environment, distances)
            ? voxel_ambient_occlusion(tracer, &extras, texel.position, texel.normal, static_cast<std::uint32_t>(index),
                                      environment, distances, stats) : 1.0F;
        VoxelSurfaceInput surface{texel.normal, normalize(subtract(gbuffer.cameraPosition, texel.position)), visibility,
                                  sunShare, gi.indirect[index], ao, caustic, transmitted};
        const Float3 mapMeters = apply_voxel_surface_maps(tracer, instance, material, environment, texel.position, texel.normal, diffuses(material, distances),
                                 distances.metersPerVoxel,
                                 length(subtract(gbuffer.cameraPosition, texel.position)) * distances.metersPerVoxel * 2.0F *
                                     std::tan(camera.verticalFieldOfViewRadians * 0.5F) / static_cast<float>(target.height),
                                 surface);
        const float footprint=length(subtract(gbuffer.cameraPosition,texel.position))*distances.metersPerVoxel*2*std::tan(camera.verticalFieldOfViewRadians*.5F)/float(target.height);
        apply_material_surface_detail(material,surface.normal,{mapMeters,multiply(transform_point(instance.transform,multiply(mapMeters,1/resolve_meters_per_voxel(distances.metersPerVoxel))),distances.metersPerVoxel),{},{},false,instance.transform,{1,0,0},{0,1,0},footprint});
        stats.layeredSurfaces += apply_voxel_material_layers(instance, material, texel.position, surface.normal, surface.ambientOcclusion, mapMeters,
                                                             distances.metersPerVoxel, footprint) > 0U ? 1U : 0U;
        if (apply_voxel_marks_and_decals(instance, extras, material, texel.position, mapMeters, footprint, surface.normal, stats) &&
            !diffuses(material, distances))
            surface.sunTerm = sun_term(surface.normal, environment, surface.visibility);
        if(material.surfaceDetail.pattern!=SurfacePattern::NoPattern && !diffuses(material, distances))surface.sunTerm=sun_term(surface.normal,environment,surface.visibility);
        const LightPasses light = shade_voxel(material, environment, surface);
        target.hdrColor[index] = beauty(light, material.baseColor.w);
        target.depth[index] = texel.normalizedDepth;
        write_voxel_passes(target, index, material, environment, surface, light,
                           camera.nearPlane + texel.normalizedDepth * (camera.farPlane - camera.nearPlane), distances.metersPerVoxel);
        target.objectId[index] = instance.objectId != 0U ? instance.objectId : instance.object->id();
        target.materialIndex[index] = texel.material;
        ++stats.hitRays;
    }
    return stats;
}
} // namespace

VoxelReferenceRenderStats ReferenceVoxelRenderer::render(
    std::span<const VoxelReferenceInstance> instances,
    const PolygonCamera& camera,
    const RenderEnvironment& environment,
    PolygonRenderTarget& target,
    bool preserveExistingDepth) const {
    VoxelReferenceRenderStats stats;
    if (!target.valid() || !(camera.nearPlane > 0.0F) ||
        !(camera.farPlane > camera.nearPlane)) return stats;
    if (!preserveExistingDepth) target.clear();

    spwi::VoxelSceneTracer tracerStorage(instances);
    {
        // Traced displacement near the camera; beyond, parallax shading (apply_voxel_surface_maps).
        const float units = resolve_meters_per_voxel(metersPerVoxel);
        tracerStorage.set_displacement(units, camera.position, environment.displacementTraceDistanceMeters / units);
    }
    const spwi::VoxelSceneTracer& tracer = tracerStorage;
    std::unique_ptr<SceneLighting> ownedLighting;
    const SceneLighting *lighting = sceneLighting;
    if (!lighting &&
        (!environment.lights.empty() || environment.pathTraceBounces > 0 || scene_uses_camera_transport(instances,occlusionPolygons.empty()?sunOccluders:occlusionPolygons))) {
      ownedLighting = std::make_unique<SceneLighting>(
          instances,
          occlusionPolygons.empty() ? sunOccluders : occlusionPolygons,
          environment, metersPerVoxel);
      lighting = ownedLighting.get();
    }
    // Metre-authored distances -> voxel units, once per frame (GPU: MetersToVoxelUnits).
    const VoxelLightingDistances distances = voxel_lighting_distances(environment, metersPerVoxel);
    SunExtras extras;
    extras.caustics = caustics;
    extras.surfaceMarks = surfaceMarks;
    extras.decals = decals;
    {
        std::vector<std::pair<const CookedPolygonAsset*, std::shared_ptr<const PolygonBvh>>> built;
        std::vector<std::pair<const CookedPolygonAsset*, std::shared_ptr<const std::vector<PolygonTextureMipChain>>>> maskedMips;
        const auto make_occluder = [&](const PolygonRenderInstance& polygon) -> std::optional<SunExtras::Occluder> {
            if (!polygon.visible || !polygon.asset) return std::nullopt;
            std::shared_ptr<const PolygonBvh> bvh;
            for (const auto& [asset, existing] : built) if (asset == polygon.asset) bvh = existing;
            if (!bvh) {
                auto fresh = std::make_shared<PolygonBvh>();
                if (!fresh->build(*polygon.asset)) return std::nullopt;
                bvh = fresh;
                built.emplace_back(polygon.asset, bvh);
            }
            SunExtras::Occluder occluder{bvh, polygon.transform, polygon.asset, polygon.tint.w, nullptr};
            if (std::any_of(polygon.asset->materials.begin(), polygon.asset->materials.end(),
                            [](const VoxelMaterialDefinition& m) { return m.blendMode == MaterialBlendMode::Masked; })) {
                for (const auto& [asset, mips] : maskedMips) if (asset == polygon.asset) occluder.maskedMips = mips;
                if (!occluder.maskedMips) {
                    auto chains = std::make_shared<std::vector<PolygonTextureMipChain>>();
                    for (const PolygonImage& image : polygon.asset->images) chains->push_back(generate_texture_mips(image));
                    occluder.maskedMips = chains;
                    maskedMips.emplace_back(polygon.asset, chains);
                }
            }
            return occluder;
        };
        for (const PolygonRenderInstance& polygon : sunOccluders)
            if (auto occluder = make_occluder(polygon)) {
                extras.occluders.push_back(*occluder);
                extras.aoOccluders.push_back(std::move(*occluder));
            }
        if (ambient_occlusion_active(environment, distances))
            for (const PolygonRenderInstance& polygon : occlusionPolygons)
                if (auto occluder = make_occluder(polygon)) extras.aoOccluders.push_back(std::move(*occluder));
    }
    if (environment.globalIlluminationMode == GlobalIlluminationMode::RadianceCascades &&
        environment.pathTraceBounces == 0 && environment.lights.empty() && !scene_uses_camera_transport(instances,occlusionPolygons.empty()?sunOccluders:occlusionPolygons))
        return render_radiance_cascades(radianceCascades, tracer, instances, camera, environment,
                                        distances, target, extras, subsurfaceDiffusionSamples);
    const auto diffuses = [&](const VoxelMaterialDefinition& m, const VoxelLightingDistances& d) {
        return subsurfaceDiffusionSamples > 0U && material_has_subsurface(m) && d.subsurfaceMaxDistance > 0.0F;
    };

    const CameraBasis basis = make_basis(camera, target);
    const float maximumDistance = camera.farPlane * 1.5F;
    stats.submittedInstances += instances.size();
    // Rows are independent: every pixel visits the instances in submission order with the
    // strict depth test and derives its sampling seed from its own index, so the image is
    // identical for any thread count (the same contract as the radiance-cascades path).
    auto rows = [&](std::uint32_t begin, std::uint32_t end, VoxelReferenceRenderStats& local) {
        for (std::uint32_t y = begin; y < end; ++y) {
            for (std::size_t k = 0; k < instances.size(); ++k) {
                const VoxelReferenceInstance& instance = instances[k];
                if (!instance.visible || instance.object == nullptr) continue;
                for (std::uint32_t x = 0; x < target.width; ++x) {
                    const Float3 ray = primary_ray(basis, x, y, target.width, target.height);
                    ++local.tracedRays;
                    // Bit-identical to raycast_voxels_transformed (dve_rc_spwi_tests), but with
                    // the tracer's dense grid and exact out-of-bounds early out, so rays that
                    // miss an instance stop at its bricks instead of marching to the far plane.
                    const auto hit = tracer.trace_instance(k, camera.position, ray, maximumDistance);
                    if (!hit) continue;
                    const float cameraZ = dot(subtract(hit->worldPosition, camera.position), basis.forward);
                    if (!(cameraZ > camera.nearPlane && cameraZ < camera.farPlane)) continue;
                    const float normalizedDepth =
                        (cameraZ - camera.nearPlane) / (camera.farPlane - camera.nearPlane);
                    const std::size_t index = static_cast<std::size_t>(y) * target.width + x;
                    if (!(normalizedDepth < target.depth[index])) continue;

                    bool fallback = false;
                    VoxelMaterialDefinition material =
                        material_for(instance, hit->objectHit.material, &fallback);
                    if (fallback) ++local.materialFallbacks;
                    const std::uint32_t seed = static_cast<std::uint32_t>(index) * 747796405U + 2891336453U;
                    const float visibility = shadow_visibility(tracer, hit->worldPosition,
                                                               hit->worldNormal, environment, distances,
                                                               seed, local, &extras);
                    const Float3 caustic = extras.caustics
                        ? extras.caustics->irradiance(hit->worldPosition, hit->worldNormal,
                                                instance.objectId != 0U ? instance.objectId : instance.object->id()) : Float3{};
                    float ao = 1.0F;
                    const Float3 indirect =
                        environment.pathTraceBounces > 0
                            ? Float3{}
                            : global_illumination(tracer, hit->worldPosition,
                                                                 hit->worldNormal, environment, distances,
                                                                 seed, local, static_cast<std::uint32_t>(index), &extras, &ao);
                    const Float3 transmitted = subsurface_light(
                        tracer, instance, hit->objectHit.material, material, hit->worldPosition, hit->worldNormal,
                        environment, distances, seed, local, &extras);
                    const float sunShare = diffuses(material, distances)
                        ? diffused_sun_term(tracer, k, hit->objectHit.material, material, hit->worldPosition,
                                            hit->worldNormal, environment, distances, subsurfaceDiffusionSamples, seed,
                                            local, &extras)
                        : sun_term(hit->worldNormal, environment, visibility);
                    VoxelSurfaceInput surface{hit->worldNormal, normalize(subtract(camera.position, hit->worldPosition)),
                                              visibility, sunShare, indirect, ao, caustic, transmitted};
                    const Float3 mapMeters = apply_voxel_surface_maps(tracer, instance, material, environment, hit->worldPosition,
                                             hit->worldNormal,
                                             diffuses(material, distances), distances.metersPerVoxel,
                                             length(subtract(camera.position, hit->worldPosition)) * distances.metersPerVoxel * 2.0F *
                                                 std::tan(camera.verticalFieldOfViewRadians * 0.5F) / static_cast<float>(target.height),
                                             surface);
                    const float footprint=length(subtract(camera.position,hit->worldPosition))*distances.metersPerVoxel*2*std::tan(camera.verticalFieldOfViewRadians*.5F)/float(target.height);
                    apply_material_surface_detail(material,surface.normal,{mapMeters,multiply(transform_point(instance.transform,multiply(mapMeters,1/resolve_meters_per_voxel(distances.metersPerVoxel))),distances.metersPerVoxel),{},{},false,instance.transform,{1,0,0},{0,1,0},footprint});
                    local.layeredSurfaces += apply_voxel_material_layers(instance, material, hit->worldPosition, surface.normal, surface.ambientOcclusion,
                                                                         mapMeters, distances.metersPerVoxel, footprint) > 0U ? 1U : 0U;
                    if (apply_voxel_marks_and_decals(instance, extras, material, hit->worldPosition, mapMeters, footprint, surface.normal, local) &&
                        !diffuses(material, distances))
                        surface.sunTerm = sun_term(surface.normal, environment, surface.visibility);
                    if(material.surfaceDetail.pattern!=SurfacePattern::NoPattern && !diffuses(material, distances))surface.sunTerm=sun_term(surface.normal,environment,surface.visibility);
                    LightPasses light = shade_voxel(material, environment, surface);
                    Float3 lightingPoint = hit->worldPosition;
                    if (lighting) {
                        auto p = world_to_voxel(instance.transform, instance.spacing, lightingPoint);
                        const auto face = hit->objectHit.normal, cell = hit->objectHit.voxel;
                        if (face.x) p.x = float(cell.x + (face.x > 0));
                        if (face.y) p.y = float(cell.y + (face.y > 0));
                        if (face.z) p.z = float(cell.z + (face.z > 0));
                        lightingPoint = voxel_to_world(instance.transform, instance.spacing, p);
                    }
                    apply_scene_lighting(light, lighting, lightingPoint,
                                         material, surface, environment, seed);
                    if (lighting && material_uses_camera_transport(material)) {
                        const float rayLimit = camera.farPlane * hit->objectHit.distance / (std::max)(cameraZ, 1e-6F);
                        light = lighting->camera_transport(camera.position,
                            normalize(subtract(hit->worldPosition, camera.position)), rayLimit,
                            seed, &material, surface.normal);
                    }
                    target.hdrColor[index] = beauty(light, material.baseColor.w);
                    target.depth[index] = normalizedDepth;
                    write_voxel_passes(target, index, material, environment, surface, light, cameraZ, distances.metersPerVoxel);
                    target.objectId[index] = instance.objectId != 0U
                        ? instance.objectId : instance.object->id();
                    target.materialIndex[index] = hit->objectHit.material;
                    ++local.hitRays;
                }
            }
        }
    };
    std::uint32_t threads = threadCount == 0U ? (std::max)(1U, std::thread::hardware_concurrency())
                                              : threadCount;
    threads = std::clamp(threads, 1U, (std::min)(16U, target.height));
    std::vector<VoxelReferenceRenderStats> partial(threads);
    if (threads == 1U) {
        rows(0U, target.height, partial[0]);
    } else {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::uint32_t t = 0; t < threads; ++t)
            workers.emplace_back(rows, target.height * t / threads, target.height * (t + 1U) / threads,
                                 std::ref(partial[t]));
        for (std::thread& worker : workers) worker.join();
    }
    for (const VoxelReferenceRenderStats& local : partial) {
        stats.tracedRays += local.tracedRays;
        stats.hitRays += local.hitRays;
        stats.materialFallbacks += local.materialFallbacks;
        stats.layeredSurfaces += local.layeredSurfaces;
        stats.markedSurfaces += local.markedSurfaces;
        stats.decalSurfaces += local.decalSurfaces;
        stats.shadowRays += local.shadowRays;
        stats.shadowBlockedRays += local.shadowBlockedRays;
        stats.globalIlluminationRays += local.globalIlluminationRays;
        stats.globalIlluminationHits += local.globalIlluminationHits;
        stats.globalIlluminationSunRays += local.globalIlluminationSunRays;
        stats.radianceCascadeIntervalRays += local.radianceCascadeIntervalRays;
        stats.subsurfaceMarches += local.subsurfaceMarches;
        stats.subsurfaceExits += local.subsurfaceExits;
        stats.subsurfaceDiffusionProbes += local.subsurfaceDiffusionProbes;
        stats.ambientOcclusionRays += local.ambientOcclusionRays;
    }
    return stats;
}

float reference_sun_visibility(std::span<const VoxelReferenceInstance> instances, Float3 point, Float3 normal,
                               const RenderEnvironment& environment, float metersPerVoxel, std::uint32_t seed) {
    spwi::VoxelSceneTracer tracer(instances);
    tracer.set_displacement(resolve_meters_per_voxel(metersPerVoxel), {}, std::numeric_limits<float>::infinity());
    const VoxelLightingDistances distances = voxel_lighting_distances(environment, metersPerVoxel);
    VoxelReferenceRenderStats stats;
    return shadow_visibility(tracer, point, normal, environment, distances, seed, stats);
}

void HybridOcclusionScene::set_displacement(float metersPerVoxel, Float3 focus, float focusRadius) noexcept {
    tracer_->set_displacement(metersPerVoxel, focus, focusRadius);
}

HybridOcclusionScene::HybridOcclusionScene(std::span<const VoxelReferenceInstance> voxels,
                                           std::span<const PolygonRenderInstance> polygons)
    : tracer_(std::make_unique<spwi::VoxelSceneTracer>(voxels)) {
    std::vector<std::pair<const CookedPolygonAsset*, std::shared_ptr<const PolygonBvh>>> built;
    for (const PolygonRenderInstance& polygon : polygons) {
        if (!polygon.visible || !polygon.asset) continue;
        std::shared_ptr<const PolygonBvh> bvh;
        for (const auto& [asset, existing] : built) if (asset == polygon.asset) bvh = existing;
        if (!bvh) {
            auto fresh = std::make_shared<PolygonBvh>();
            if (!fresh->build(*polygon.asset)) continue;
            bvh = fresh;
            built.emplace_back(polygon.asset, bvh);
        }
        polygons_.emplace_back(bvh, polygon.transform);
    }
}

HybridOcclusionScene::~HybridOcclusionScene() = default;

std::optional<float> HybridOcclusionScene::nearest(Float3 origin, Float3 direction, float maximumDistance) const {
    std::optional<float> best;
    if (const auto hit = tracer_->trace(origin, direction, maximumDistance)) best = hit->objectHit.distance;
    for (const auto& [bvh, pose] : polygons_)
        if (const auto hit = bvh->raycast(inverse_transform_point(pose, origin), inverse_transform_vector(pose, direction),
                                          best ? *best : maximumDistance))
            if (!best || hit->distance < *best) best = hit->distance;
    return best;
}

AmbientOcclusionRay HybridOcclusionScene::ray() const {
    return [this](Float3 origin, Float3 direction, float maximumDistance) { return nearest(origin, direction, maximumDistance); };
}

HybridReferenceRenderStats render_hybrid_reference(
    std::span<const VoxelReferenceInstance> voxels,
    std::span<const PolygonRenderInstance> polygons,
    const PolygonCamera& camera,
    const RenderEnvironment& environment,
    PolygonRenderTarget& target,
    const PolygonRenderOptions& polygonOptions,
    float metersPerVoxel,
    std::uint32_t voxelThreadCount) {
    HybridReferenceRenderStats stats;
    std::unique_ptr<SceneLighting> lighting;
    if (!environment.lights.empty() || environment.pathTraceBounces > 0 || scene_uses_camera_transport(voxels,polygons))
      lighting = std::make_unique<SceneLighting>(voxels, polygons, environment,
                                                 metersPerVoxel);
    ReferenceVoxelRenderer voxelRenderer;
    voxelRenderer.sceneLighting = lighting.get();
    voxelRenderer.metersPerVoxel = metersPerVoxel;
    voxelRenderer.threadCount = voxelThreadCount;
    voxelRenderer.radianceCascades.threadCount = voxelThreadCount;
    voxelRenderer.sunOccluders = polygons;
    voxelRenderer.occlusionPolygons = polygons;   // voxels' AO rays see the polygons too
    stats.voxels = voxelRenderer.render(voxels, camera, environment, target, false);
    (void)paint_sky_background(target, camera, environment);   // before polygons, so glass blends over it
    PolygonRenderOptions options = polygonOptions;
    options.sceneLighting = lighting.get();
    options.preserveExistingDepth = true;
    options.metersPerUnit = metersPerVoxel;
    // Polygons' AO rays see the voxels too.
    std::optional<HybridOcclusionScene> occlusion;
    if (!options.occlusionRay && environment.ambientOcclusionSamples > 0U &&
        environment.globalIlluminationMode != GlobalIlluminationMode::Off) {
        occlusion.emplace(voxels, polygons);
        occlusion->set_displacement(resolve_meters_per_voxel(metersPerVoxel), camera.position,
                                    environment.displacementTraceDistanceMeters / resolve_meters_per_voxel(metersPerVoxel));
        options.occlusionRay = occlusion->ray();
    }
    stats.polygons = ReferencePolygonRenderer{}.render(polygons, camera, environment, target, options);
    (void)apply_aerial_perspective(target, camera, environment, metersPerVoxel);
    if (environment.volumetricFogDensityPerMeter > 0.0F) {
        // Light shafts: the fog's shadow rays see voxels and polygons alike.
        const HybridOcclusionScene* scene = occlusion ? &*occlusion : nullptr;
        std::optional<HybridOcclusionScene> own;
        if (!scene) {
            own.emplace(voxels, polygons);
            own->set_displacement(resolve_meters_per_voxel(metersPerVoxel), camera.position,
                                  environment.displacementTraceDistanceMeters / resolve_meters_per_voxel(metersPerVoxel));
            scene = &*own;
        }
        stats.volumetricFog = apply_volumetric_fog(target, camera, environment, metersPerVoxel, scene->ray(), 0U, voxelThreadCount);
    }
    (void)apply_distance_fog(target, camera, environment, metersPerVoxel);
    return stats;
}

} // namespace dve::render
