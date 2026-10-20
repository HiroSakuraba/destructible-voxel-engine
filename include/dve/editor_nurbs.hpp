#pragma once
#include "dve/nurbs.hpp"
#include "dve/editor_command.hpp"
namespace dve::editor {
inline constexpr std::string_view kNurbsComponent="dve.nurbs";
[[nodiscard]] bool has_nurbs_source(const EditorObject&) noexcept;
[[nodiscard]] std::optional<NurbsSource> editor_nurbs_source(const EditorObject&,std::string* error=nullptr);
[[nodiscard]] bool validate_editor_nurbs(const EditorObject&,std::string* error=nullptr);
[[nodiscard]] std::optional<EditorObject> make_editor_nurbs(EditorObjectId,const NurbsSource&,std::string* error=nullptr);
class EditNurbsCommand final:public IEditorCommand {
public:
    EditNurbsCommand(EditorObjectId,NurbsSource before,NurbsSource after);
    std::string_view label() const noexcept override{return "Edit NURBS control";}
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&,const NurbsSource& expected,const NurbsSource& value);
    EditorObjectId id_;NurbsSource before_,after_;
};
}

