#pragma once
#include "dve/editor_polygon.hpp"
#include "dve/subdivision_surface.hpp"
namespace dve::editor {
enum class CageEdgeTraversal : std::uint8_t { Loop, Ring };
enum class CageEdgeStop : std::uint8_t { Boundary, Pole, Triangle };
struct CageEdgeSelectionResult {
    std::set<std::uint64_t> edges;
    std::set<CageEdgeStop> stops;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// Uses saved control faces, never render-triangle diagonals. Multiple seeds return their union.
// Loops continue through regular quad vertices (or along a regular boundary); rings cross
// opposite quad edges. Unsupported continuations stop, malformed topology refuses the edit.
// Invalid seeds or budget exhaustion return no partial selection. Geometry is never changed.
[[nodiscard]] CageEdgeSelectionResult select_cage_edge_chain(const CookedPolygonAsset&,
    const SubdivisionSettings&, const std::set<std::uint64_t>& seeds, CageEdgeTraversal,
    std::size_t maximumEdges = kMaxSubdivisionControlTriangles * 3);
struct CageLoopCutResult {
    PolygonEditResult polygon;
    SubdivisionSettings subdivision;
    PolygonTopologyLineage lineage;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(polygon); }
};
// A single cut across the ring through seed. Fraction is measured from the seed's
// smaller canonical endpoint and propagated with orientation through the quad strip.
// Touched faces must be convex planar quads with compatible attributes on their diagonal.
[[nodiscard]] CageLoopCutResult cut_cage_edge_ring(const CookedPolygonAsset&,
    const SubdivisionSettings&, std::uint64_t seed, float fraction = .5F);
// Returns the cage for ordinary polygons and level zero; otherwise caches the evaluated surface.
[[nodiscard]] const CookedPolygonAsset* editor_surface_mesh(const EditorObject& object);
[[nodiscard]] std::uint32_t subdivision_control_triangle(const EditorObject&, std::uint32_t evaluatedTriangle);
class SetSubdivisionSettingsCommand final : public IEditorCommand {
public:
    SetSubdivisionSettingsCommand(EditorObjectId id, SubdivisionSettings before, SubdivisionSettings after);
    std::string_view label() const noexcept override { return "Subdivision surface settings"; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, SubdivisionSettings expected, SubdivisionSettings value);
    EditorObjectId id_{};
    SubdivisionSettings before_{},after_{};
    bool captured_{};
    std::optional<ComponentValue> levelOverride_,sharpOverride_,geometryOverride_;
};
}
