#pragma once

// Light transport through glass and water for the CPU reference: a scene of voxel instances and
// polygon meshes that a ray can enter, cross and leave, bending at every surface by Snell's law.
//
// Materials with transmission > 0 (material_is_refractive) are media. At their surface light is
// split by the exact dielectric Fresnel equations: a reflected part and a transmitted part (times
// `transmission`; the rest of an only partly transmissive material is absorbed here, as that
// light lights the surface itself and is the direct-sun term). Inside, it is tinted by
// Beer-Lambert absorption toward attenuationColor. Total internal reflection is followed. Every
// other surface is a receiver: the light path ends there.
//
// Voxel media are blocky (cell faces); polygon media use their interpolated vertex normals, so a
// smooth mesh is a smooth lens. Coordinates are the reference renderer's world units (voxel index
// units of rigid instances).

#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <span>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"

namespace dve::render {

namespace spwi { class VoxelSceneTracer; }

struct OpticalPolygonInstance {
    std::uint64_t objectId{};
    const CookedPolygonAsset* asset{};
    RigidTransform transform{};
    bool visible{true};
};

struct OpticalHit {
    float distance{};
    Float3 position{};
    Float3 geometricNormal{};   // world, the surface's own outward side (for voxels: the face hit)
    Float3 shadingNormal{};     // world; interpolated for polygons, the face normal for voxels
    const VoxelMaterialDefinition* material{};
    std::uint64_t objectId{};
    std::int32_t voxelInstance{-1};   // index into the voxel instances, or -1 for polygons
    std::int32_t polygonInstance{-1};
    Int3 voxel{};                     // the cell hit (voxel instances)
    MaterialId materialId{};          // voxel instances
};

// Exact unpolarised dielectric Fresnel reflectance for light going from index n1 into n2 at
// cos(incidence) = cosI (>= 0). 1 under total internal reflection.
[[nodiscard]] float dielectric_fresnel(float cosI, float n1, float n2) noexcept;
// Refraction of unit `direction` through a surface with unit `normal` facing against it
// (dot(direction, normal) < 0), eta = n1 / n2. nullopt under total internal reflection.
[[nodiscard]] std::optional<Float3> refract_direction(Float3 direction, Float3 normal, float eta) noexcept;
[[nodiscard]] Float3 reflect_direction(Float3 direction, Float3 normal) noexcept;
// Beer-Lambert transmittance after `distance` (world units) of travel inside `material`;
// metersPerUnit converts the distance to the metres of attenuationDistanceMeters.
[[nodiscard]] Float3 medium_transmittance(const VoxelMaterialDefinition& material, float distance,
                                          float metersPerUnit) noexcept;

class OpticalScene {
public:
    OpticalScene(std::span<const VoxelReferenceInstance> voxels, std::span<const OpticalPolygonInstance> polygons,
                 float metersPerUnit = kDefaultMetersPerVoxel);
    ~OpticalScene();
    OpticalScene(const OpticalScene&) = delete;
    OpticalScene& operator=(const OpticalScene&) = delete;

    // Nearest surface along the ray: any voxel cell (outside a medium) or polygon triangle.
    [[nodiscard]] std::optional<OpticalHit> trace(Float3 origin, Float3 direction, float maximumDistance) const;
    // From a point inside a voxel medium (`inside` is the entry hit), the next boundary along the
    // ray: where this instance's cells stop being that material, or a nearer surface of anything
    // else. The returned hit's material is what lies beyond (nullptr: empty space).
    [[nodiscard]] std::optional<OpticalHit> trace_inside_voxels(const OpticalHit& inside, Float3 origin, Float3 direction,
                                                                float maximumDistance) const;
    // World bounds of the refractive objects (empty when there are none).
    [[nodiscard]] bool refractive_bounds(Float3& minimum, Float3& maximum) const;
    // World bounds of everything.
    [[nodiscard]] bool bounds(Float3& minimum, Float3& maximum) const;
    [[nodiscard]] float meters_per_unit() const noexcept { return metersPerUnit_; }

private:
    struct PolygonData;
    [[nodiscard]] std::optional<OpticalHit> trace_polygons(Float3 origin, Float3 direction, float maximumDistance) const;
    std::span<const VoxelReferenceInstance> voxels_;
    std::unique_ptr<spwi::VoxelSceneTracer> tracer_;
    std::vector<PolygonData> polygons_;
    float metersPerUnit_{kDefaultMetersPerVoxel};
};

// Sunlight followed forward from `origin` along `direction` through refractive media until it
// lands on a receiver (an opaque or diffuse surface).
struct LightPathSettings {
    std::uint32_t maximumInteractions{12};
    float maximumDistance{1.0e5F};
    float surfaceOffset{1.0e-4F};   // world units, along the new direction after each interface
};
struct LightPathResult {
    bool landed{};                 // reached a receiver
    Float3 position{};
    Float3 normal{};               // receiver's surface normal, facing the arriving light
    Float3 direction{};            // arrival direction
    Float3 throughput{1.0F, 1.0F, 1.0F};
    std::uint32_t refractions{};   // interfaces crossed (0: direct light)
    std::uint32_t reflections{};   // specular reflections (Fresnel or total internal)
    std::uint64_t receiverObject{};
    float pathLength{};            // from the last interface to the receiver (world units)
};
// Deterministic: at each interface only the transmitted branch is followed, weighted by
// (1 - Fresnel) x transmission (total internal reflection is followed with weight 1).
[[nodiscard]] LightPathResult follow_light_transmitted(const OpticalScene& scene, Float3 origin, Float3 direction,
                                                       const LightPathSettings& settings = {});
// Stochastic: at each interface reflection or transmission is chosen with the Fresnel
// probability (throughput stays unbiased); `random` returns uniform [0, 1).
template <typename Random>
[[nodiscard]] LightPathResult follow_light_sampled(const OpticalScene& scene, Float3 origin, Float3 direction,
                                                   Random&& random, const LightPathSettings& settings = {});

// Implementation detail shared by both: `choose(F)` returns true to reflect.
[[nodiscard]] LightPathResult follow_light_impl(const OpticalScene& scene, Float3 origin, Float3 direction,
                                                const LightPathSettings& settings, bool sampled,
                                                float (*uniform)(void*), void* state);

template <typename Random>
LightPathResult follow_light_sampled(const OpticalScene& scene, Float3 origin, Float3 direction, Random&& random,
                                     const LightPathSettings& settings) {
    using R = std::remove_reference_t<Random>;
    return follow_light_impl(scene, origin, direction, settings, true,
                             [](void* state) -> float { return static_cast<float>((*static_cast<R*>(state))()); },
                             static_cast<void*>(&random));
}

} // namespace dve::render
