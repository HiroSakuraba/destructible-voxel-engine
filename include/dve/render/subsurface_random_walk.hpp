#pragma once

// Ground truth for subsurface scattering: light followed through the volume of a Subsurface
// material as a random walk (Chiang, Kutz and Burley, "Practical and Controllable Subsurface
// Scattering for Production Path Tracing", SIGGRAPH 2016 talk).
//
// The medium: extinction sigma_t and single-scattering albedo alpha, isotropic scattering (or
// Henyey-Greenstein with g). A walk enters at a surface point, flies exponential free paths, at
// each event survives with probability alpha and scatters, and ends where it crosses the
// surface again (exits) or is absorbed. A smooth boundary with index eta reflects back inside
// with the exact Fresnel probability (total internal reflection included); eta = 1 is
// index-matched.
//
// The medium for a material: Chiang et al.'s fits from the artist's albedo A (per channel) and
// scatter distance d, the same two numbers Burley's profile uses:
//   alpha   = 1 - exp(-5.09406 A + 2.61188 A^2 - 4.31805 A^3)
//   s(A)    = 1.9 - A + 3.5 (A - 0.8)^2
//   sigma_t = 1 / (d s(A))
//
// The walk runs inside any SubsurfaceDomain: an analytic slab (for exact checks), the cells of
// one material in a voxel instance, or a closed polygon mesh. All in world units.

#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>

#include "dve/render/subsurface.hpp"

namespace dve::render {

// One colour channel's medium, in world units.
struct SubsurfaceMedium {
    float extinction{1.0F};        // sigma_t, per world unit
    float albedo{0.9F};            // single-scattering albedo
    float anisotropy{0.0F};        // Henyey-Greenstein g
    float boundaryIndex{1.0F};     // medium / outside; 1 = index-matched
};

// Chiang et al. 2016: the medium that reproduces an artist's albedo A and Burley scatter
// distance (metres), converted to world units with metersPerUnit.
[[nodiscard]] SubsurfaceMedium subsurface_medium_from_albedo(float albedo, float scatterDistanceMeters,
                                                             float metersPerUnit) noexcept;

// The volume a walk lives in. march() is the same contract as march_voxel_subsurface /
// march_polygon_subsurface: from a point inside, along a direction, the first boundary within
// maximumDistance (found = false: none that close).
class SubsurfaceDomain {
public:
    virtual ~SubsurfaceDomain() = default;
    [[nodiscard]] virtual SubsurfaceExit march(Float3 point, Float3 direction, float maximumDistance) const = 0;
};

// bottom <= y <= top, unbounded in x and z (top = +inf or bottom = -inf for a half-space).
class SlabSubsurfaceDomain final : public SubsurfaceDomain {
public:
    SlabSubsurfaceDomain(float bottom, float top) noexcept : bottom_(bottom), top_(top) {}
    [[nodiscard]] SubsurfaceExit march(Float3 point, Float3 direction, float maximumDistance) const override;
private:
    float bottom_, top_;
};

// The cells of one material in a voxel instance. Leaving into anything but air absorbs.
class VoxelSubsurfaceDomain final : public SubsurfaceDomain {
public:
    VoxelSubsurfaceDomain(const VoxelReferenceInstance& instance, MaterialId material) noexcept
        : instance_(&instance), material_(material) {}
    [[nodiscard]] SubsurfaceExit march(Float3 point, Float3 direction, float maximumDistance) const override;
private:
    const VoxelReferenceInstance* instance_;
    MaterialId material_;
};

// The inside of a closed mesh.
class PolygonSubsurfaceDomain final : public SubsurfaceDomain {
public:
    PolygonSubsurfaceDomain(const PolygonBvh& bvh, const CookedPolygonAsset& asset, const RigidTransform& transform) noexcept
        : bvh_(&bvh), asset_(&asset), transform_(transform) {}
    [[nodiscard]] SubsurfaceExit march(Float3 point, Float3 direction, float maximumDistance) const override;
private:
    const PolygonBvh* bvh_;
    const CookedPolygonAsset* asset_;
    RigidTransform transform_;
};

struct RandomWalkSettings {
    std::uint32_t maximumEvents{1U << 16};   // scattering events before the walk is dropped
    float surfaceOffset{1.0e-4F};             // world units, off the surface on entry and reflection
};

struct RandomWalkResult {
    enum class End : std::uint8_t { Exited, Absorbed, Blocked, Dropped } end{End::Dropped};
    Float3 position{};      // where it left (Exited)
    Float3 normal{};        // outward surface normal there
    Float3 direction{};     // outgoing direction, outside the medium
    std::uint32_t events{}; // scattering events
};

// Uniform [0, 1) source; any callable returning double or float.
template <typename Random>
[[nodiscard]] RandomWalkResult random_walk_subsurface(const SubsurfaceDomain& domain, const SubsurfaceMedium& medium,
                                                      Float3 entryPoint, Float3 entryNormal, Float3 direction,
                                                      Random&& random, const RandomWalkSettings& settings = {});

// Light that arrives from outside along `incoming` (pointing into the surface): the share
// reflected specularly at the boundary (Fresnel) is not part of the walk. Returns nullopt for
// that share; otherwise the walk from the refracted direction.
template <typename Random>
[[nodiscard]] std::optional<RandomWalkResult> enter_and_walk_subsurface(const SubsurfaceDomain& domain,
                                                                        const SubsurfaceMedium& medium, Float3 entryPoint,
                                                                        Float3 entryNormal, Float3 incoming, Random&& random,
                                                                        const RandomWalkSettings& settings = {});

// Implementation detail shared by the templates.
[[nodiscard]] RandomWalkResult random_walk_impl(const SubsurfaceDomain& domain, const SubsurfaceMedium& medium,
                                                Float3 entryPoint, Float3 entryNormal, Float3 direction,
                                                float (*uniform)(void*), void* state, const RandomWalkSettings& settings);
[[nodiscard]] std::optional<RandomWalkResult> enter_and_walk_impl(const SubsurfaceDomain& domain,
                                                                  const SubsurfaceMedium& medium, Float3 entryPoint,
                                                                  Float3 entryNormal, Float3 incoming,
                                                                  float (*uniform)(void*), void* state,
                                                                  const RandomWalkSettings& settings);

template <typename Random>
RandomWalkResult random_walk_subsurface(const SubsurfaceDomain& domain, const SubsurfaceMedium& medium, Float3 entryPoint,
                                        Float3 entryNormal, Float3 direction, Random&& random,
                                        const RandomWalkSettings& settings) {
    using R = std::remove_reference_t<Random>;
    return random_walk_impl(domain, medium, entryPoint, entryNormal, direction,
                            [](void* s) -> float { return static_cast<float>((*static_cast<R*>(s))()); },
                            static_cast<void*>(&random), settings);
}

template <typename Random>
std::optional<RandomWalkResult> enter_and_walk_subsurface(const SubsurfaceDomain& domain, const SubsurfaceMedium& medium,
                                                          Float3 entryPoint, Float3 entryNormal, Float3 incoming,
                                                          Random&& random, const RandomWalkSettings& settings) {
    using R = std::remove_reference_t<Random>;
    return enter_and_walk_impl(domain, medium, entryPoint, entryNormal, incoming,
                               [](void* s) -> float { return static_cast<float>((*static_cast<R*>(s))()); },
                               static_cast<void*>(&random), settings);
}

} // namespace dve::render
