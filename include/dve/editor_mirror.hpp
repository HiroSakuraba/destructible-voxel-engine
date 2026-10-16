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

// Half-model mirroring (Polygon > Mirror Mesh / Mirror Selected Faces): reflect triangles across
// the mesh's own plane through its local origin (normal = local axis) and append them, welding
// vertices that lie on the plane (within `tolerance`) so the two halves share the seam.
//   faces == nullptr: the whole mesh. Triangles lying entirely on the plane are removed (they would
//     sit inside the closed result). The mesh must lie on one side of the plane.
//   faces != nullptr: only those triangles are copied; the rest of the mesh is untouched.
// The lineage keeps every original element's identity; the mirrored half is new.
struct MeshMirrorResult {
    CookedPolygonAsset mesh;
    std::optional<SubdivisionSettings> subdivision;
    PolygonTopologyLineage lineage;
    std::set<std::uint32_t> newFaces;      // triangle indices of the mirrored copies
    std::size_t welded{};                  // seam vertices shared by both halves
    std::size_t removed{};                 // on-plane triangles removed (whole mesh)
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
[[nodiscard]] MeshMirrorResult mirror_polygon_half(const CookedPolygonAsset& source, const SubdivisionSettings* subdivision,
                                                   int localAxis, const std::set<std::uint32_t>* faces, float tolerance);

}  // namespace dve::editor
