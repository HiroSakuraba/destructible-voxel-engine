#pragma once
#include "dve/editor_document.hpp"
#include "dve/editor_viewport.hpp"
#include <filesystem>
#include <string>
namespace dve::editor {
[[nodiscard]] EditorDocument make_prototype_starter();
[[nodiscard]] EditorDocument make_racing_starter();
[[nodiscard]] EditorDocument make_flying_starter();
[[nodiscard]] EditorDocument make_water_starter();
// True when the exporter writes the object as a bodyless runtime marker (an empty voxel asset
// that carries components). Polygon, text, Gabor and .dmesh-backed objects are never markers.
[[nodiscard]] bool prototype_object_is_marker(const EditorObject&);
[[nodiscard]] bool validate_editor_prototype(const EditorDocument&,std::string* error=nullptr);
[[nodiscard]] bool export_prototype_project(const EditorDocument&,const EditorMaterialLibrary&,const std::filesystem::path&,std::string* error=nullptr);
struct PrototypePanelState {bool open{},picking{};std::size_t role{},component{},first{};EditorObjectId editingObject{};ComponentId editingComponent{};std::string editingProperty,typed,message;bool replaceText{true};};
struct PrototypePanelLayout {UiRect panel,close,previous,next;std::vector<UiRect> rows;};
}
