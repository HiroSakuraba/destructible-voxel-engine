#pragma once
#include "dve/editor_nurbs.hpp"
#include "dve/nurbs_attachment.hpp"
#include "dve/nurbs_tessellation.hpp"
namespace dve::editor {
inline constexpr std::string_view kNurbsSeamProperty="seam";
struct NurbsSeam {
    EditorObjectId reference{},moving{};
    NurbsAttachmentOptions options;
    std::uint64_t referenceStamp{},movingStamp{};
    NurbsDisplayGrid grid;
    std::vector<double> fractions;
    std::vector<Float3> anchors;
};
// Same parent, local pose and axis scale, with no socket attachment.
[[nodiscard]] bool nurbs_seam_same_frame(const EditorObject&,const EditorObject&);
[[nodiscard]] bool has_nurbs_seam(const EditorObject&) noexcept;
[[nodiscard]] std::optional<NurbsSeam> editor_nurbs_seam(const EditorObject&,std::string* error=nullptr);
[[nodiscard]] bool validate_editor_nurbs_seams(const EditorDocument&,std::string* error=nullptr);
[[nodiscard]] std::optional<CookedPolygonAsset> tessellate_editor_nurbs_display(const EditorObject&,const NurbsSource&,std::string*,std::stop_token={});
[[nodiscard]] std::unique_ptr<IEditorCommand> link_nurbs_seam_command(EditorObjectId reference,EditorObjectId moving,NurbsAttachmentOptions);
[[nodiscard]] std::unique_ptr<IEditorCommand> unlink_nurbs_seam_command(EditorObjectId);
[[nodiscard]] std::unique_ptr<IEditorCommand> refresh_nurbs_seam_command(EditorObjectId);
[[nodiscard]] std::unique_ptr<IEditorCommand> edit_nurbs_seam_command(EditorObjectId,const NurbsSource& before,const NurbsSource& after);
}
