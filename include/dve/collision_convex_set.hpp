#pragma once

// Exact convex collision: a set of convex prisms whose union is the part.
//
// A polygon object's one convex hull (dve.collision_hull) or bounding box fills
// in every hole. The `dve.collision_convex_set` component instead lists convex
// prisms (brepkernel's certified pieces of an extruded CAD part), so a hole stays
// a hole for the physics engine. Precedence at runtime: an enabled convex set,
// then an enabled authored hull, then the bounding box.
//
// Pieces are in the object's mesh-local frame in the editor (meters, before
// voxelScale) and baked by voxelScale for runtime scenes and Play, like hulls.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/component.hpp"
#include "dve/rigid_body_adapter.hpp"

namespace dve {

inline constexpr std::string_view kConvexSetComponent = "dve.collision_convex_set";
inline constexpr std::size_t kConvexSetMaxPieces = 4096;
inline constexpr std::size_t kConvexSetMaxRing = 64;

struct ConvexPrism {
    std::vector<Float3> bottom;    // ring of n points
    std::vector<Float3> top;       // the same ring moved along the extrusion axis
    [[nodiscard]] std::vector<Float3> points() const;
};

[[nodiscard]] bool validate_convex_set(std::span<const ConvexPrism> pieces) noexcept;
[[nodiscard]] std::string encode_convex_set(std::span<const ConvexPrism> pieces);
[[nodiscard]] std::optional<std::vector<ConvexPrism>> decode_convex_set(std::string_view text);
[[nodiscard]] std::optional<std::vector<ConvexPrism>> convex_set_component_pieces(const Component& component);
// Pieces of the first enabled convex set component, or empty.
[[nodiscard]] std::vector<ConvexPrism> active_convex_set(std::span<const Component> components);
[[nodiscard]] Component bake_convex_set_component(Component component, Float3 metric);

struct ConvexSetMassProperties {
    double volumeCubicMeters{};
    Float3 centerOfMass{};
    // For unit density (multiply by kg/m^3); about the center of mass, tensor convention
    // (off-diagonal entries are minus the products of inertia).
    SolverInertiaTensor inertiaPerDensity{};
};
// Exact for prisms (closed-surface tetrahedra in double precision).
[[nodiscard]] std::optional<ConvexSetMassProperties> convex_set_mass_properties(
    std::span<const ConvexPrism> pieces) noexcept;

} // namespace dve
