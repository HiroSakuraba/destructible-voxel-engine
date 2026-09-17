#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class InspectingCanvas final : public IEditorCanvas {
public:
    void fill(UiRect, EditorColor) const override { ++fills; }
    void outline(UiRect, EditorColor) const override { ++outlines; }
    void line(int, int, int, int, EditorColor, int) const override { ++lines; }
    void text(int, int, std::string_view value, EditorColor) const override {
        ++texts;
        if (value.find("3D RENDERING DIAGNOSTICS") != std::string_view::npos) sawTitle = true;
        if (value.find("Triangles ") != std::string_view::npos) sawGeometry = true;
        if (value.find("Skinned vertices ") != std::string_view::npos) sawSkinning = true;
        if (value.find("Depth complexity ") != std::string_view::npos) sawDepth = true;
        if (value.find("Occlusion culled ") != std::string_view::npos) sawOcclusion = true;
        if (value.find("Voxel material modes ") != std::string_view::npos) sawVoxelMaterials = true;
    }
    [[nodiscard]] int text_width(std::string_view value) const override {
        return static_cast<int>(value.size()) * 7;
    }
    mutable int fills{};
    mutable int outlines{};
    mutable int lines{};
    mutable int texts{};
    mutable bool sawTitle{};
    mutable bool sawGeometry{};
    mutable bool sawSkinning{};
    mutable bool sawDepth{};
    mutable bool sawOcclusion{};
    mutable bool sawVoxelMaterials{};
};
}

int main() {
    try {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.resize(1280, 800);
        const MenuAction* action = controller.workspace().menus().find("render.diagnostics3d");
        require(action != nullptr, "3D diagnostics menu action is missing");
        require(action->shortcut == "F11", "3D diagnostics shortcut is wrong");
        require(controller.dispatch_action("render.diagnostics3d"), "3D diagnostics action failed");
        require(controller.render3d_diagnostics_open(), "3D diagnostics panel did not open");
        const Render3DDiagnosticsReport& report = controller.render3d_diagnostics_report();
        require(report.drawCallCount == 4U && report.pipelineBreaks.size() == 3U,
                "native panel did not retain the deterministic reference report");
        require(report.voxelMaterials.brickCount == 5U &&
                report.voxelMaterials.recookRequestCount == 1U,
                "native panel did not retain voxel material policy evidence");
        InspectingCanvas canvas;
        render_native_editor(canvas, controller, 1280, 800);
        require(canvas.sawTitle && canvas.sawGeometry && canvas.sawSkinning &&
                canvas.sawDepth && canvas.sawOcclusion && canvas.sawVoxelMaterials && canvas.fills > 20,
                "native 3D diagnostics panel did not render the required sections");
        require(controller.dispatch_action("render.diagnostics3d") &&
                !controller.render3d_diagnostics_open(), "3D diagnostics panel did not close");
        std::cout << "editor render3d diagnostics v2.23 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
