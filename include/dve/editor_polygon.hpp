#pragma once
#include "dve/editor_command.hpp"
#include "dve/editor_viewport.hpp"
#include "dve/polygon_bvh.hpp"
#include <span>
#include <array>

namespace dve::editor {
// Geometric vertex/edge identity spans split UV and normal seams. Faces are triangles.
enum class PolygonSelectionMode : std::uint8_t { Vertex, Edge, Face };
enum class PolygonSelectionEdit : std::uint8_t { Grow, Shrink, Connected };
struct PolygonSelection {
    EditorObjectId object{};
    PolygonSelectionMode mode{PolygonSelectionMode::Face};
    std::set<std::uint64_t> components;
};
struct PolygonEdge {
    std::uint32_t a{}, b{};
    [[nodiscard]] std::uint64_t key() const noexcept { return (std::uint64_t(a)<<32U)|b; }
};
enum class PolygonEdit : std::uint8_t {
    Translate, Rotate, Scale, Delete, Duplicate, Extrude, Inset, Bevel,
    RecalculateNormals, FlipNormals, ProjectUV, Weld, AssignMaterial
};
struct PolygonEditSettings {
    Float3 vector{};
    Float3 pivot{};
    Quaternion rotation{};
    float amount{0.1F};
    float inset{0.2F};
    std::uint32_t material{};
    std::uint32_t uvAxis{2};
    std::uint64_t maximumVertices{1000000};
    std::uint64_t maximumIndices{6000000};
};
struct PolygonEditResult {
    CookedPolygonAsset mesh;
    PolygonSelection selection;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
// Operation-specific lineage; IDs here are scoped to the before/after mesh arrays.
// This is not a persistent global topology-ID scheme or a nearest-position remap.
struct PolygonTopologyLineage {
    std::map<std::uint32_t,std::uint32_t> vertexSurvivors;
    std::map<std::uint64_t,std::set<std::uint64_t>> edgeDescendants;
    std::map<std::uint32_t,std::set<std::uint32_t>> cornerDescendants;
    std::map<std::uint32_t,std::vector<std::uint32_t>> controlFaceDescendants;
};
[[nodiscard]] CookedPolygonAsset make_editor_polygon_cube(EditorObjectId id,float size=1);
[[nodiscard]] CookedPolygonAsset make_editor_polygon_plane(EditorObjectId id,float size=4);
void refresh_editor_polygon(CookedPolygonAsset&,bool normals=false);
[[nodiscard]] CookedPolygonAsset bake_editor_polygon(const EditorObject&, bool controlCage = false);
[[nodiscard]] std::vector<PolygonEdge> polygon_edges(const CookedPolygonAsset&);
[[nodiscard]] std::set<std::uint32_t> polygon_selected_vertices(const CookedPolygonAsset&,const PolygonSelection&);
[[nodiscard]] std::set<std::uint32_t> polygon_selected_faces(const CookedPolygonAsset&,const PolygonSelection&);
[[nodiscard]] bool polygon_selection_valid(const CookedPolygonAsset&,const PolygonSelection&);
[[nodiscard]] std::set<std::uint64_t> polygon_all_components(const CookedPolygonAsset&,PolygonSelectionMode);
// Conversion selects incident components; face-to-edge keeps only edges of selected faces.
[[nodiscard]] PolygonSelection convert_polygon_selection(const CookedPolygonAsset&,const PolygonSelection&,PolygonSelectionMode);
// Faces connect across shared geometric edges; vertices/edges connect at their endpoints.
// Shrink removes components adjacent to an unselected component; a full mesh stays selected.
[[nodiscard]] std::set<std::uint64_t> edit_polygon_selection(const CookedPolygonAsset&,const PolygonSelection&,PolygonSelectionEdit);
[[nodiscard]] PolygonEditResult edit_polygon(const CookedPolygonAsset&,const PolygonSelection&,PolygonEdit,const PolygonEditSettings& = {});
// Cuts interpolate vertex attributes. Caps support simple contours without nested holes.
[[nodiscard]] PolygonEditResult slice_polygon(const CookedPolygonAsset&,Float3 point,Float3 normal,bool front,bool cap=true);
[[nodiscard]] PolygonEditResult join_editor_polygons(const EditorDocument&,EditorObjectId primary,std::span<const EditorObjectId>, bool evaluated = false);
[[nodiscard]] CookedPolygonAsset extract_polygon_faces(const CookedPolygonAsset&,const PolygonSelection&);
[[nodiscard]] std::vector<CookedPolygonAsset> separate_polygon_components(const CookedPolygonAsset&);
[[nodiscard]] std::optional<PolygonBvhHit> pick_editor_polygon(const EditorObject&,ViewportRay,float maximum=10000,bool controlCage=false);
[[nodiscard]] std::optional<std::uint64_t> pick_polygon_component(const EditorObject&,const EditorCamera&,UiRect,int x,int y,PolygonSelectionMode,bool through=false);
[[nodiscard]] std::set<std::uint64_t> polygon_components_in_rect(const EditorObject&,const EditorCamera&,UiRect,UiRect,PolygonSelectionMode,bool through=false,bool crossing=false,
    const EditorDocument* scene=nullptr,const std::set<EditorObjectId>* onlyObjects=nullptr);
class ReplacePolygonCommand final : public IEditorCommand {
public:
    ReplacePolygonCommand(EditorObjectId id,CookedPolygonAsset before,CookedPolygonAsset after,std::string label,std::uint64_t stroke=0,
        std::optional<SubdivisionSettings> authoredAfter = {});
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
    [[nodiscard]] bool merge_with(const IEditorCommand&) override;
private:
    CommandResult apply(EditorDocument&,const CookedPolygonAsset&,const std::optional<SubdivisionSettings>&);
    EditorObjectId id_{};
    std::uint64_t stroke_{};
    CookedPolygonAsset before_,after_;
    std::optional<SubdivisionSettings> beforeSubdivision_,afterSubdivision_;
    bool explicitSubdivision_{};
    std::string label_;
    bool capturedOverride_{};
    std::optional<ComponentValue> previousOverride_;
};
struct EditorPolygonDrawTriangle {
    EditorObjectId object{};
    std::uint32_t face{};
    std::array<ScreenPoint,3> points;
    Float4 color{};
    float depth{};
    float facing{1.0F}; // |cos| between the face normal and the view direction, for flat shading
    bool selected{};
};
// Bounded software preview. Runtime poses are optional; production textures use the player renderer.
[[nodiscard]] std::vector<EditorPolygonDrawTriangle> build_editor_polygon_draw_list(
    const EditorDocument&,const EditorCamera&,UiRect,const std::set<EditorObjectId>& selected,
    const std::set<EditorObjectId>& isolated,std::size_t maximum=100000,
    const std::map<EditorObjectId,RigidTransform>* poses=nullptr,EditorObjectId cagePreview=0);
} // namespace dve::editor
