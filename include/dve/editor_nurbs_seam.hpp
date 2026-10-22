#pragma once
#include "dve/editor_nurbs.hpp"
#include "dve/nurbs_attachment.hpp"
#include "dve/nurbs_tessellation.hpp"
#include <cstddef>
#include <memory>
#include <set>
#include <stop_token>
#include <string>
#include <vector>
namespace dve::editor {
// Immutable connected-network snapshot; workers never retain a document or controller pointer.
struct NurbsSeamDragSnapshot {
    EditorObject reference, moving;
    std::vector<EditorObject> additional;
    [[nodiscard]] std::vector<const EditorObject*> objects() const;
    [[nodiscard]] const EditorObject* find(EditorObjectId) const;
};
[[nodiscard]] bool nurbs_seam_snapshot_matches(const EditorDocument&,const NurbsSeamDragSnapshot&,bool editable=true);
[[nodiscard]] bool nurbs_seam_controls_driven(const EditorObject&,const std::set<std::size_t>&);
[[nodiscard]] std::shared_ptr<const NurbsSeamDragSnapshot> capture_nurbs_seam_drag(
    const EditorDocument&,EditorObjectId,std::string* error=nullptr);
class PreparedNurbsSeamEdit final : public IEditorCommand {
public:
    static std::unique_ptr<PreparedNurbsSeamEdit> prepare(
        std::shared_ptr<const NurbsSeamDragSnapshot>,EditorObjectId,const NurbsSource&,
        std::string* error=nullptr,std::stop_token={});
    std::string_view label() const noexcept override { return "Transform linked NURBS network"; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
    [[nodiscard]] bool matches(const EditorDocument&,bool after,bool editable=true) const;
    [[nodiscard]] bool publish_preview(EditorDocument&,const PreparedNurbsSeamEdit* previous,std::string* error);
    [[nodiscard]] bool restore_preview(EditorDocument&,std::string* error);
    [[nodiscard]] const EditorObject& edited_object(EditorObjectId id) const { return *after_.find(id); }
private:
    bool publish(EditorDocument&,bool after,std::string* error);
    std::shared_ptr<const NurbsSeamDragSnapshot> before_;
    NurbsSeamDragSnapshot after_;
};
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
[[nodiscard]] std::optional<std::vector<NurbsSeam>> editor_nurbs_seams(const EditorObject&,std::string* error=nullptr);
[[nodiscard]] std::vector<EditorObjectId> nurbs_seam_network_ids(const EditorDocument&,EditorObjectId,std::string* error=nullptr);
[[nodiscard]] bool can_link_nurbs_seams(const EditorDocument&,EditorObjectId,EditorObjectId,NurbsAttachmentOptions,std::string* error=nullptr);
// Legacy accessor: first incident seam. Use editor_nurbs_seams for network operations.
[[nodiscard]] std::optional<NurbsSeam> editor_nurbs_seam(const EditorObject&,std::string* error=nullptr);
[[nodiscard]] bool validate_editor_nurbs_seams(const EditorDocument&,std::string* error=nullptr);
[[nodiscard]] std::optional<CookedPolygonAsset> tessellate_editor_nurbs_display(const EditorObject&,const NurbsSource&,std::string*,std::stop_token={});
[[nodiscard]] std::unique_ptr<IEditorCommand> link_nurbs_seam_command(EditorObjectId reference,EditorObjectId moving,NurbsAttachmentOptions);
[[nodiscard]] std::unique_ptr<IEditorCommand> unlink_nurbs_seam_command(EditorObjectId);
[[nodiscard]] std::unique_ptr<IEditorCommand> refresh_nurbs_seam_command(EditorObjectId);
[[nodiscard]] std::unique_ptr<IEditorCommand> edit_nurbs_seam_command(EditorObjectId,const NurbsSource& before,const NurbsSource& after);
}
