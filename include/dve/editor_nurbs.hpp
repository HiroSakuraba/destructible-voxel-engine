#pragma once
#include "dve/nurbs.hpp"
#include "dve/editor_command.hpp"
#include <memory>
#include <stop_token>
namespace dve::editor {
inline constexpr std::string_view kNurbsComponent="dve.nurbs";
[[nodiscard]] bool has_nurbs_source(const EditorObject&) noexcept;
[[nodiscard]] std::optional<NurbsSource> editor_nurbs_source(const EditorObject&,std::string* error=nullptr);
[[nodiscard]] bool validate_editor_nurbs(const EditorObject&,std::string* error=nullptr,std::stop_token stop={});
[[nodiscard]] std::optional<EditorObject> make_editor_nurbs(EditorObjectId,const NurbsSource&,std::string* error=nullptr);
[[nodiscard]] bool replace_editor_nurbs_source(EditorObject&,const NurbsSource&,std::string* error=nullptr);
// Compares authored display geometry without traversing texture/image resources.
[[nodiscard]] bool nurbs_display_geometry_matches(const CookedPolygonAsset&,const CookedPolygonAsset&);
[[nodiscard]] std::uint64_t nurbs_source_stamp(const NurbsSource&);
// The object's source with its controls mapped into world space (world pose and axis scale),
// in double precision; weights are unchanged, so the mapped rational geometry is exact.
// Use it to compare curvature or continuity between objects, or for world-unit measurements.
[[nodiscard]] std::optional<NurbsSource> world_editor_nurbs_source(const EditorDocument&,const EditorObject&,std::string* error=nullptr);
class EditNurbsCommand final:public IEditorCommand {
public:
    EditNurbsCommand(EditorObjectId,NurbsSource before,NurbsSource after);
    // Prepares and validates immutable before/after meshes away from the UI thread.
    static std::unique_ptr<EditNurbsCommand> prepare(const EditorObject&,const NurbsSource&,std::string*,std::stop_token={});
    [[nodiscard]] const CookedPolygonAsset* prepared_mesh() const {return afterMesh_?&*afterMesh_:nullptr;}
    // Cheap stale-cache guard; does not re-tessellate.
    [[nodiscard]] bool matches_before(const EditorObject&) const;
    std::string_view label() const noexcept override{return "Edit NURBS source";}
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&,const NurbsSource& expected,const NurbsSource& value);
    EditorObjectId id_;NurbsSource before_,after_;
    std::optional<CookedPolygonAsset> beforeMesh_,afterMesh_;
    std::unique_ptr<IEditorCommand> seamEdit_;
};
}

