#pragma once
#include "dve/editor_polygon.hpp"
#include <span>

namespace dve::editor {
enum class MeshBoolean : std::uint8_t { Union, Difference, ReverseDifference, Intersection };
[[nodiscard]] bool mesh_boolean_available() noexcept;
[[nodiscard]] PolygonEditResult boolean_editor_polygons(const EditorDocument&,EditorObjectId a,EditorObjectId b,MeshBoolean);
struct TopologyDiagnostics {
    std::size_t boundaries{},nonmanifold{},degenerate{},inconsistentWinding{};
    std::set<std::uint64_t> problemEdges;
};
[[nodiscard]] TopologyDiagnostics diagnose_polygon_topology(const CookedPolygonAsset&);
enum class TopologyEdit : std::uint8_t { Subdivide, SplitEdges, FlipDiagonal, BridgeLoops, CapBoundaries, HardNormals, SmoothNormals, BevelConvexEdges };
[[nodiscard]] PolygonEditResult edit_polygon_topology(const CookedPolygonAsset&,const PolygonSelection&,TopologyEdit,float amount=.5F);
[[nodiscard]] PolygonEditResult bevel_convex_editor_edges(const CookedPolygonAsset&,const PolygonSelection&,float width);
[[nodiscard]] PolygonEditResult convex_editor_hull(EditorObjectId,std::span<const Float3>);
[[nodiscard]] std::vector<std::set<std::uint32_t>> polygon_uv_islands(const CookedPolygonAsset&,std::uint32_t channel=0);
enum class UvEdit : std::uint8_t { Translate, Rotate, Scale, Split, Stitch, Unwrap, Pack, FlipU, FlipV };
struct UvEditSettings { Float2 vector{}; Float2 pivot{.5F,.5F}; float angle{}; float padding{.02F}; std::uint32_t channel{}; std::map<std::uint32_t,Float2> pins; };
// IDs identify triangle corners (face*3+corner), so shared vertex attributes never pull an
// unselected face through a UV edit. UV0 and UV1 are independent.
[[nodiscard]] PolygonEditResult edit_polygon_uv(const CookedPolygonAsset&,const std::set<std::uint32_t>& corners,UvEdit,const UvEditSettings& = {});
} // namespace dve::editor
