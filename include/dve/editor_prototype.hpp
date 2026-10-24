#pragma once
#include "dve/editor_document.hpp"
#include "dve/editor_viewport.hpp"
#include <filesystem>
namespace dve::editor {
[[nodiscard]] EditorDocument make_prototype_starter();
[[nodiscard]] bool validate_editor_prototype(const EditorDocument&,std::string* error=nullptr);
[[nodiscard]] bool export_prototype_project(const EditorDocument&,const EditorMaterialLibrary&,const std::filesystem::path&,std::string* error=nullptr);
struct PrototypePanelState {bool open{},picking{};std::size_t role{},component{},first{};ComponentId editingComponent{};std::string editingProperty,typed,message;bool replaceText{true};};
struct PrototypePanelLayout {UiRect panel,close,previous,next;std::vector<UiRect> rows;};
}
