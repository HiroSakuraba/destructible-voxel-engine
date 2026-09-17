#include "dve/render/voxel_reference_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include "dve/query.hpp"

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
Float3 lerp3(Float3 a, Float3 b, float t) noexcept { return add3(multiply(a, 1.0F-t), multiply(b, t)); }

struct CameraBasis {
    Float3 right{};
    Float3 up{};
    Float3 forward{};
    float tanHalf{};
    float aspect{};
};

struct SceneHit {
    const VoxelReferenceInstance* instance{};
    TransformedRayHit hit{};
    const VoxelMaterialDefinition* material{};
};

CameraBasis make_basis(const PolygonCamera& camera, const PolygonRenderTarget& target) noexcept {
    CameraBasis basis;
    basis.forward = normalize(subtract(camera.target, camera.position));
    basis.right = normalize(cross3(basis.forward, camera.up));
    if (length_squared(basis.right) < kEpsilon) basis.right = {1.0F, 0.0F, 0.0F};
    basis.up = normalize(cross3(basis.right, basis.forward));
    basis.tanHalf = std::tan(camera.verticalFieldOfViewRadians * 0.5F);
    basis.aspect = static_cast<float>(target.width) / static_cast<float>(target.height);
    return basis;
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

std::optional<SceneHit> trace_scene(std::span<const VoxelReferenceInstance> instances,
                                    Float3 origin, Float3 direction, float maximumDistance) {
    std::optional<SceneHit> nearest;
    float nearestDistance = maximumDistance;
    for (const VoxelReferenceInstance& instance : instances) {
        if (!instance.visible || instance.object == nullptr) continue;
        const auto hit = raycast_voxels_transformed(*instance.object, instance.transform,
                                                     origin, direction, nearestDistance);
        if (!hit || hit->objectHit.distance >= nearestDistance) continue;
        nearestDistance = hit->objectHit.distance;
        nearest = SceneHit{&instance, *hit, &material_for(instance, hit->objectHit.material)};
    }
    return nearest;
}

Float3 environment_radiance(const RenderEnvironment& environment, Float3 direction) noexcept {
    const Float3 normalized = normalize(direction);
    return lerp3(environment.groundColor, environment.skyColor,
                 std::clamp(0.5F + 0.5F * normalized.y, 0.0F, 1.0F));
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

float hard_visibility(std::span<const VoxelReferenceInstance> instances, Float3 origin,
                      Float3 direction, float maximumDistance, VoxelReferenceRenderStats& stats) {
    ++stats.shadowRays;
    if (trace_scene(instances, origin, direction, maximumDistance)) {
        ++stats.shadowBlockedRays;
        return 0.0F;
    }
    return 1.0F;
}

float shadow_visibility(std::span<const VoxelReferenceInstance> instances, Float3 point,
                        Float3 normal, const RenderEnvironment& environment,
                        std::uint32_t pixelSeed, VoxelReferenceRenderStats& stats) {
    if (environment.shadowMode == ShadowMode::Off) return 1.0F;
    const Float3 sun = normalize(environment.sunDirection);
    const Float3 origin = add(point, multiply(normal, environment.shadowBiasMeters));
    auto weighted = [&](float raw) {
        return std::clamp(1.0F - environment.shadowStrength * (1.0F - raw), 0.0F, 1.0F);
    };
    if (environment.shadowMode == ShadowMode::Hard)
        return weighted(hard_visibility(instances, origin, sun,
                                        environment.shadowMaxDistanceMeters, stats));
    if (environment.shadowMode == ShadowMode::Contact)
        return weighted(hard_visibility(instances, origin, sun,
                                        environment.contactShadowDistanceMeters, stats));

    const std::uint32_t samples = std::clamp(environment.shadowSamples, 1U, 16U);
    float visible = 0.0F;
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
        const Float3 direction = sample_sun_disk(sun, environment.shadowSoftnessRadians,
                                                 sample, samples, pixelSeed);
        visible += hard_visibility(instances, origin, direction,
                                   environment.shadowMaxDistanceMeters, stats);
    }
    visible /= static_cast<float>(samples);
    if (environment.shadowMode == ShadowMode::Hybrid) {
        const float contact = hard_visibility(instances, origin, sun,
                                              environment.contactShadowDistanceMeters, stats);
        visible = std::min(visible, contact);
    }
    return weighted(visible);
}

Float3 global_illumination(std::span<const VoxelReferenceInstance> instances, Float3 point,
                           Float3 normal, const RenderEnvironment& environment,
                           std::uint32_t pixelSeed, VoxelReferenceRenderStats& stats) {
    if (environment.globalIlluminationMode == GlobalIlluminationMode::Off) return {};
    if (environment.globalIlluminationMode == GlobalIlluminationMode::AmbientHemisphere)
        return multiply(environment_radiance(environment, normal), environment.globalIlluminationIntensity);

    const std::uint32_t samples = std::clamp(environment.globalIlluminationSamples, 1U, 16U);
    const Float3 origin = add(point, multiply(normal, environment.shadowBiasMeters));
    Float3 accumulated{};
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
        const Float3 direction = cosine_hemisphere(normal, sample, samples, pixelSeed);
        ++stats.globalIlluminationRays;
        const auto hit = trace_scene(instances, origin, direction,
                                     environment.globalIlluminationMaxDistanceMeters);
        if (!hit) {
            accumulated = add3(accumulated, environment_radiance(environment, direction));
            continue;
        }
        ++stats.globalIlluminationHits;
        const VoxelMaterialDefinition& bounce = *hit->material;
        const Float3 bounceColor{bounce.baseColor.x, bounce.baseColor.y, bounce.baseColor.z};
        const Float3 localAmbient = environment_radiance(environment, hit->hit.worldNormal);
        Float3 incoming = multiply3(bounceColor, multiply(localAmbient, 1.0F - bounce.metallic));
        incoming = add3(incoming, bounce.emissive);
        accumulated = add3(accumulated, incoming);
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

    const CameraBasis basis = make_basis(camera, target);
    const float maximumDistance = camera.farPlane * 1.5F;
    for (const VoxelReferenceInstance& instance : instances) {
        ++stats.submittedInstances;
        if (!instance.visible || instance.object == nullptr) continue;
        for (std::uint32_t y = 0; y < target.height; ++y) {
            const float ndcY = 1.0F - 2.0F *
                ((static_cast<float>(y) + 0.5F) / static_cast<float>(target.height));
            for (std::uint32_t x = 0; x < target.width; ++x) {
                const float ndcX = 2.0F *
                    ((static_cast<float>(x) + 0.5F) / static_cast<float>(target.width)) - 1.0F;
                Float3 ray = basis.forward;
                ray = add(ray, multiply(basis.right, ndcX * basis.tanHalf * basis.aspect));
                ray = add(ray, multiply(basis.up, ndcY * basis.tanHalf));
                ray = normalize(ray);
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
                const float visibility = shadow_visibility(instances, hit->worldPosition,
                                                           hit->worldNormal, environment, seed, stats);
                const Float3 indirect = global_illumination(instances, hit->worldPosition,
                                                             hit->worldNormal, environment, seed, stats);
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
    const PolygonRenderOptions& polygonOptions) {
    HybridReferenceRenderStats stats;
    stats.voxels = ReferenceVoxelRenderer{}.render(voxels, camera, environment, target, false);
    PolygonRenderOptions options = polygonOptions;
    options.preserveExistingDepth = true;
    stats.polygons = ReferencePolygonRenderer{}.render(polygons, camera, environment, target, options);
    return stats;
}

} // namespace dve::render
