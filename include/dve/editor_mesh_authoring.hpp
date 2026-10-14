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
enum class BevelOffsetType : std::uint8_t { Fractional, Absolute };
struct EdgeBevelSettings {
    float width{0.2F};
    BevelOffsetType offsetType{BevelOffsetType::Fractional};
    bool worldSpace{true};
    std::uint32_t segments{1};
    float depth{1.0F}; // 0 = flat chamfer; 1 = circular convex profile
    bool autoFit{true}; // conservative shortest-edge width cap
    float smoothingAngle{30.0F};
};
[[nodiscard]] PolygonEditResult bevel_convex_editor_edges(const CookedPolygonAsset&,const PolygonSelection&,
    const EdgeBevelSettings&,Float3 objectScale={1,1,1});
// Compatibility overload: original mesh-local bisector-plane offset and flat chamfer.
[[nodiscard]] PolygonEditResult bevel_convex_editor_edges(const CookedPolygonAsset&,const PolygonSelection&,float width);
[[nodiscard]] PolygonEditResult convex_editor_hull(EditorObjectId,std::span<const Float3>);
struct PolygonUvAuthoring {
    bool enabled{true};
    std::uint64_t layoutHash{};
    std::array<std::set<std::uint64_t>,2> seams;
    std::array<std::set<std::uint32_t>,2> pins;
};
[[nodiscard]] std::vector<std::uint32_t> polygon_geometric_vertices(const CookedPolygonAsset&);
[[nodiscard]] std::uint64_t polygon_uv_layout_hash(const CookedPolygonAsset&);
[[nodiscard]] std::optional<PolygonUvAuthoring> read_polygon_uv_authoring(const EditorObject&,std::string* error=nullptr);
// Returns the encoded after record, or an empty string when no saved component exists.
// Retired seams/pins without supported lineage refuse the edit instead of silently clearing.
[[nodiscard]] std::optional<PolygonUvAuthoring> remap_polygon_uv_authoring(const PolygonUvAuthoring&,
    const CookedPolygonAsset& after, const PolygonTopologyLineage&, std::string* error=nullptr);
[[nodiscard]] std::optional<std::string> remap_polygon_uv_authoring_record(const EditorObject&,
    const CookedPolygonAsset& after, const PolygonTopologyLineage&, std::string* error=nullptr);
[[nodiscard]] std::unique_ptr<IEditorCommand> make_polygon_uv_authoring_command(
    EditorDocument&,const EditorObject&,const PolygonUvAuthoring&,std::string* error=nullptr);
[[nodiscard]] std::vector<std::set<std::uint32_t>> polygon_uv_islands(
    const CookedPolygonAsset&,std::uint32_t channel=0,const std::set<std::uint64_t>& seams={});
enum class UvEdit : std::uint8_t { Translate, Rotate, Scale, Split, Stitch, Unwrap, Pack, FlipU, FlipV };
struct UvEditSettings { Float2 vector{}; Float2 pivot{.5F,.5F}; float angle{}; float padding{.02F}; std::uint32_t channel{}; std::map<std::uint32_t,Float2> pins; std::set<std::uint64_t> seams; };
// IDs identify triangle corners (face*3+corner), so shared vertex attributes never pull an
// unselected face through a UV edit. UV0 and UV1 are independent.
[[nodiscard]] PolygonEditResult edit_polygon_uv(const CookedPolygonAsset&,const std::set<std::uint32_t>& corners,UvEdit,const UvEditSettings& = {});
} // namespace dve::editor
