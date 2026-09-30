#include "dve/render/voxel_reference_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include "dve/query.hpp"
#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/render/ray_lighting_reference.hpp"

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

Float3 sample_sun_disk(Float3 sunDirection, float angularRadius, std::uint32_t sample,
                       std::uint32_t count, std::uint32_t rotationSeed) noexcept {
    if (!(angularRadius > 0.0F)) return sunDirection;
    const float u = (static_cast<float>(sample) + 0.5F) / static_cast<float>(count);
    const float v = std::fmod(radical_inverse(sample ^ rotationSeed) +
                              radical_inverse(rotationSeed), 1.0F);
    const float radius = std::sqrt(u) * std::tan(angularRadius);
    const float angle = 2.0F * kPi * v;
    Float3 tangent{}, bitangent{};
    make_basis(sunDirection, tangent, bitangent);
    return normalize(add3(sunDirection,
        add3(multiply(tangent, radius * std::cos(angle)),
             multiply(bitangent, radius * std::sin(angle)))));
}

// Secondary rays go through spwi::VoxelSceneTracer, whose hits are bit-identical to
// raycast_voxels_transformed over the same instances (tested in dve_rc_spwi_tests).
float hard_visibility(const spwi::VoxelSceneTracer& tracer, Float3 origin,
                      Float3 direction, float maximumDistance, VoxelReferenceRenderStats& stats) {
    ++stats.shadowRays;
    if (tracer.occluded(origin, direction, maximumDistance)) {
        ++stats.shadowBlockedRays;
        return 0.0F;
    }
    return 1.0F;
}

float shadow_visibility(const spwi::VoxelSceneTracer& tracer, Float3 point,
                        Float3 normal, const RenderEnvironment& environment,
                        const VoxelLightingDistances& distances, std::uint32_t pixelSeed, VoxelReferenceRenderStats& stats) {
    if (environment.shadowMode == ShadowMode::Off) return 1.0F;
    const Float3 sun = normalize(environment.sunDirection);
    const Float3 origin = add(point, multiply(normal, distances.shadowBias));
    auto weighted = [&](float raw) {
        return std::clamp(1.0F - environment.shadowStrength * (1.0F - raw), 0.0F, 1.0F);
    };
    if (environment.shadowMode == ShadowMode::Hard)
        return weighted(hard_visibility(tracer, origin, sun,
                                        distances.shadowMaxDistance, stats));
    if (environment.shadowMode == ShadowMode::Contact)
        return weighted(hard_visibility(tracer, origin, sun,
                                        distances.contactShadowDistance, stats));

    const std::uint32_t samples = std::clamp(environment.shadowSamples, 1U, 16U);
    float visible = 0.0F;
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
        const Float3 direction = sample_sun_disk(sun, environment.shadowSoftnessRadians,
                                                 sample, samples, pixelSeed);
        visible += hard_visibility(tracer, origin, direction,
                                   distances.shadowMaxDistance, stats);
    }
    visible /= static_cast<float>(samples);
    if (environment.shadowMode == ShadowMode::Hybrid) {
        const float contact = hard_visibility(tracer, origin, sun,
                                              distances.contactShadowDistance, stats);
        visible = std::min(visible, contact);
    }
    return weighted(visible);
}

Float3 global_illumination(const spwi::VoxelSceneTracer& tracer, Float3 point,
                           Float3 normal, const RenderEnvironment& environment,
                           const VoxelLightingDistances& distances, std::uint32_t pixelSeed, VoxelReferenceRenderStats& stats) {
    if (environment.globalIlluminationMode == GlobalIlluminationMode::Off) return {};
    if (environment.globalIlluminationMode == GlobalIlluminationMode::AmbientHemisphere)
        return multiply(environment_radiance(environment, normal), environment.globalIlluminationIntensity);

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

Float4 shade_voxel(const VoxelMaterialDefinition& material, Float3 worldNormal,
                   const RenderEnvironment& environment, float visibility,
                   Float3 indirect) noexcept {
    if (material.shadingModel == MaterialShadingModel::Unlit) return material.baseColor;
    const Float3 light = normalize(environment.sunDirection);
    const float ndotl = std::max(0.0F, dot(worldNormal, light));
    const Float3 direct = multiply(environment.sunColor,
                                   environment.sunIntensity * ndotl * visibility);
    const Float3 base{material.baseColor.x, material.baseColor.y, material.baseColor.z};
    Float3 lit = multiply3(base, add3(direct, multiply(indirect, 1.0F - material.metallic)));
    lit = add3(lit, material.emissive);
    if (material.shadingModel == MaterialShadingModel::Emissive)
        lit = add3(base, material.emissive);
    return {lit.x, lit.y, lit.z, material.baseColor.w};
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
    std::uint32_t threads = threadCount == 0U ? std::max(1U, std::thread::hardware_concurrency())
                                              : threadCount;
    threads = std::clamp(threads, 1U, std::min(16U, height));
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
    PolygonRenderTarget& target) {
    VoxelReferenceRenderStats stats;
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
        const VoxelMaterialDefinition& material = material_for(instance, texel.material, &fallback);
        if (fallback) ++stats.materialFallbacks;
        const std::uint32_t seed = static_cast<std::uint32_t>(index) * 747796405U + 2891336453U;
        const float visibility = shadow_visibility(tracer, texel.position, texel.normal,
                                                   environment, distances, seed, stats);
        target.hdrColor[index] = shade_voxel(material, texel.normal, environment, visibility,
                                             gi.indirect[index]);
        target.depth[index] = texel.normalizedDepth;
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

    const spwi::VoxelSceneTracer tracer(instances);
    // Metre-authored distances -> voxel units, once per frame (GPU: MetersToVoxelUnits).
    const VoxelLightingDistances distances = voxel_lighting_distances(environment, metersPerVoxel);
    if (environment.globalIlluminationMode == GlobalIlluminationMode::RadianceCascades)
        return render_radiance_cascades(radianceCascades, tracer, instances, camera, environment,
                                        distances, target);

    const CameraBasis basis = make_basis(camera, target);
    const float maximumDistance = camera.farPlane * 1.5F;
    for (const VoxelReferenceInstance& instance : instances) {
        ++stats.submittedInstances;
        if (!instance.visible || instance.object == nullptr) continue;
        for (std::uint32_t y = 0; y < target.height; ++y) {
            for (std::uint32_t x = 0; x < target.width; ++x) {
                const Float3 ray = primary_ray(basis, x, y, target.width, target.height);
                ++stats.tracedRays;
                const auto hit = raycast_voxels_transformed(*instance.object, instance.transform,
                                                             camera.position, ray, maximumDistance);
                if (!hit) continue;
                const float cameraZ = dot(subtract(hit->worldPosition, camera.position), basis.forward);
                if (!(cameraZ > camera.nearPlane && cameraZ < camera.farPlane)) continue;
                const float normalizedDepth =
                    (cameraZ - camera.nearPlane) / (camera.farPlane - camera.nearPlane);
                const std::size_t index = static_cast<std::size_t>(y) * target.width + x;
                if (!(normalizedDepth < target.depth[index])) continue;

                bool fallback = false;
                const VoxelMaterialDefinition& material =
                    material_for(instance, hit->objectHit.material, &fallback);
                if (fallback) ++stats.materialFallbacks;
                const std::uint32_t seed = static_cast<std::uint32_t>(index) * 747796405U + 2891336453U;
                const float visibility = shadow_visibility(tracer, hit->worldPosition,
                                                           hit->worldNormal, environment, distances,
                                                           seed, stats);
                const Float3 indirect = global_illumination(tracer, hit->worldPosition,
                                                             hit->worldNormal, environment, distances,
                                                             seed, stats);
                target.hdrColor[index] = shade_voxel(material, hit->worldNormal,
                                                     environment, visibility, indirect);
                target.depth[index] = normalizedDepth;
                target.objectId[index] = instance.objectId != 0U
                    ? instance.objectId : instance.object->id();
                target.materialIndex[index] = hit->objectHit.material;
                ++stats.hitRays;
            }
        }
    }
    return stats;
}

HybridReferenceRenderStats render_hybrid_reference(
    std::span<const VoxelReferenceInstance> voxels,
    std::span<const PolygonRenderInstance> polygons,
    const PolygonCamera& camera,
    const RenderEnvironment& environment,
    PolygonRenderTarget& target,
    const PolygonRenderOptions& polygonOptions,
    float metersPerVoxel) {
    HybridReferenceRenderStats stats;
    ReferenceVoxelRenderer voxelRenderer;
    voxelRenderer.metersPerVoxel = metersPerVoxel;
    stats.voxels = voxelRenderer.render(voxels, camera, environment, target, false);
    PolygonRenderOptions options = polygonOptions;
    options.preserveExistingDepth = true;
    stats.polygons = ReferencePolygonRenderer{}.render(polygons, camera, environment, target, options);
    return stats;
}

} // namespace dve::render
