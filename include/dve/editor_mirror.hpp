#pragma once

// Mirroring (artist request, 15 Oct 2026): reflect objects across a world plane, as a copy or in
// place, with real geometry flips. Objects stay rigid (no negative scale): a reflection M of an
// object with world transform T is written as M∘T = T'∘F_e, where F_e flips one local axis e of
// the geometry and T' is an ordinary rigid transform.
//
// Voxel cells flip exactly on their grid (cell i -> -i-1 along e); polygon meshes and subdivision
// cages flip positions and normals along e and reverse triangle winding, keeping their persistent
// element IDs, control faces and creases.

#include <optional>
#include <string>

#include "dve/editor_command.hpp"
#include "dve/editor_modeling_topology.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/subdivision_surface.hpp"

namespace dve::editor {

struct MirroredPose {
    RigidTransform transform;  // T'
    int localAxis{};           // e: 0 = X, 1 = Y, 2 = Z of the object's own geometry
};

// Reflect a world transform across the plane through `point` whose normal is world axis `axis`.
[[nodiscard]] MirroredPose mirror_pose(const RigidTransform& transform, int axis, Float3 point) noexcept;

// Flip voxel cells (and anchors) along a local axis: cell i -> -i-1, so the occupied volume
// reflects exactly about the local origin plane.
[[nodiscard]] VoxelObjectState mirror_voxel_state(const VoxelObjectState& state, int localAxis);

// Flip a polygon mesh (render mesh or subdivision cage) along a local axis about the local origin:
// positions and normals mirror, triangles reverse winding. Optional subdivision settings and
// modeling topology are updated to match (control faces reverse, corner IDs follow). Bounds,
// tangents and the content hash are refreshed.
[[nodiscard]] bool mirror_polygon_mesh(CookedPolygonAsset& mesh, int localAxis, SubdivisionSettings* subdivision,
                                       ModelingTopology* topology, std::string* error = nullptr);

// Lineage for mirror_polygon_mesh: every element survives; the two swapped corners of each
// triangle trade places. Used to carry persistent IDs and saved UV seams/pins.
[[nodiscard]] PolygonTopologyLineage mirror_polygon_lineage(const CookedPolygonAsset& before, const SubdivisionSettings* subdivision);

}  // namespace dve::editor
