#pragma once
#include "dve/editor_polygon.hpp"
#include "dve/subdivision_surface.hpp"
namespace dve::editor {
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
    std::optional<ComponentValue> levelOverride_,sharpOverride_;
};
}
