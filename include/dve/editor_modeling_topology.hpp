#pragma once
#include "dve/subdivision_surface.hpp"
#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <span>
namespace dve::editor {
using ModelingId = std::uint64_t;
struct PolygonTopologyLineage;
struct PolygonSelection;
struct EditorObject;
// Object-scoped editor metadata. Render and cooked asset formats remain unchanged.
struct ModelingTopology {
    ModelingId nextId{1};
    std::vector<ModelingId> vertices; // geometric ID for each attribute-array entry
    std::vector<ModelingId> attributes, corners, triangles, faces;
    std::map<std::pair<ModelingId,ModelingId>,ModelingId> edges; // includes render diagonals
    std::vector<std::vector<ModelingId>> faceTriangles;
    std::set<ModelingId> controlEdges; // selectable authored edges; render diagonals remain separate
    auto operator<=>(const ModelingTopology&) const = default;
};
struct ModelingChange {
    ModelingTopology topology;
    std::map<ModelingId,std::set<ModelingId>> descendants; // empty means retired
    std::set<ModelingId> created;
    std::string error;
    explicit operator bool() const noexcept {return error.empty();}
};
[[nodiscard]] std::optional<ModelingTopology> make_modeling_topology(const CookedPolygonAsset&,
    const SubdivisionSettings* = nullptr, std::span<const std::uint32_t> geometricPartition = {}, std::string* error=nullptr);
[[nodiscard]] bool validate_modeling_topology(const CookedPolygonAsset&,const SubdivisionSettings*,const ModelingTopology&,std::string* error=nullptr);
[[nodiscard]] bool modeling_matches_legacy_partition(const CookedPolygonAsset&,const ModelingTopology&);
[[nodiscard]] PolygonTopologyLineage identity_polygon_lineage(const CookedPolygonAsset&,const SubdivisionSettings* = nullptr);
[[nodiscard]] ModelingChange advance_modeling_topology(const CookedPolygonAsset& before,const CookedPolygonAsset& after,
    const SubdivisionSettings* afterSettings,const ModelingTopology&,const PolygonTopologyLineage&,ModelingId allocationFloor=1);
[[nodiscard]] ModelingChange change_modeling_faces(const CookedPolygonAsset&,const SubdivisionSettings* before,
    const SubdivisionSettings* after,const ModelingTopology&,ModelingId allocationFloor=1,
    const CookedPolygonAsset* output=nullptr);
[[nodiscard]] bool ensure_modeling_topology(EditorObject&,std::string* error=nullptr);
[[nodiscard]] std::set<ModelingId> modeling_selection_ids(const CookedPolygonAsset&,const ModelingTopology&,const PolygonSelection&);
[[nodiscard]] std::set<std::uint64_t> resolve_modeling_selection(const CookedPolygonAsset&,const ModelingTopology&,std::uint8_t mode,const std::set<ModelingId>&);
void write_modeling_topology(std::ostream&,const ModelingTopology&);
[[nodiscard]] std::optional<ModelingTopology> read_modeling_topology(std::istream&,std::string* error=nullptr);
[[nodiscard]] std::uint64_t modeling_topology_hash(const ModelingTopology&) noexcept;
}
