#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

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
        if (value.find("TILE WORLD EDITOR") != std::string_view::npos) sawTileEditor = true;
        if (value.find("AUTOTILE / PARALLAX / CHUNKS") != std::string_view::npos) sawTileDiagnostics = true;
        if (value.find("LEVEL VALID") != std::string_view::npos) sawValidLevel = true;
        if (value.find("SPRITE DIAGNOSTICS") != std::string_view::npos) sawSpriteDiagnostics = true;
        if (value.find("Draws ") != std::string_view::npos) sawDrawCounts = true;
        if (value.find("Atlas ") != std::string_view::npos) sawAtlas = true;
    }
    [[nodiscard]] int text_width(std::string_view value) const override {
        return static_cast<int>(value.size()) * 7;
    }
    mutable int fills{};
    mutable int outlines{};
    mutable int lines{};
    mutable int texts{};
    mutable bool sawTileEditor{};
    mutable bool sawTileDiagnostics{};
    mutable bool sawValidLevel{};
    mutable bool sawSpriteDiagnostics{};
    mutable bool sawDrawCounts{};
    mutable bool sawAtlas{};
};
}

int main() {
    using namespace dve;
    using namespace dve::editor;
    try {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.configure_ai_assistant(DVE_SOURCE_DIR);
        controller.resize(1280, 800);
        require(controller.workspace().menus().find("sprite.tile_world") != nullptr,
                "Tile World Editor menu action is missing");
        require(controller.workspace().menus().find("sprite.diagnostics") != nullptr,
                "Sprite Diagnostics menu action is missing");
        require(controller.dispatch_action("sprite.tile_world"),
                "Tile World Editor action failed");
        require(controller.tile_world_editor_open(), "Tile World Editor did not open");
        const auto tileFrame = controller.tile_world_editor().frame(
            controller.tile_world_canvas_viewport(), controller.tile_world_palette_viewport(), 2U);
        require(tileFrame.validForPlay && !tileFrame.layers.empty() && !tileFrame.chunks.empty(),
                "native tile editor did not expose validation, layers, and chunks");
        InspectingCanvas tileCanvas;
        render_native_editor(tileCanvas, controller, 1280, 800);
        require(tileCanvas.sawTileEditor && tileCanvas.sawTileDiagnostics &&
                tileCanvas.sawValidLevel && tileCanvas.fills > 30,
                "native canvas did not render the complete tile-world panel");

        controller.key_down("7", false, false, false);
        require(controller.tile_world_editor().canvas().tool() == TileMapTool::CollisionShape,
                "tile tool keyboard selection failed");
        controller.key_down("a", false, false, false);
        require(!controller.tile_world_editor().show_autotile_rules(),
                "autotile visualization toggle failed");
        controller.key_down("a", false, false, false);
        controller.key_down("f5", false, false, false);
        require(!controller.tile_world_editor_open() && controller.sprite_level_playing(),
                "one-click play test did not launch the deterministic sprite level");

        require(controller.dispatch_action("sprite.diagnostics"),
                "Sprite Diagnostics action failed");
        require(controller.sprite_diagnostics_open(), "Sprite Diagnostics did not enable");
        for (int frame = 0; frame < 3; ++frame) controller.update(1.0F / 60.0F);
        InspectingCanvas diagnosticsCanvas;
        render_native_editor(diagnosticsCanvas, controller, 1280, 800);
        require(diagnosticsCanvas.sawSpriteDiagnostics && diagnosticsCanvas.sawDrawCounts &&
                diagnosticsCanvas.sawAtlas && diagnosticsCanvas.fills > 50,
                "native sprite diagnostics panel did not render counts, atlas, and heatmap content");

        controller.key_down("escape", false, false, false);
        require(!controller.sprite_level_playing(), "Escape did not stop play test");
        std::cout << "v2.19 editor diagnostics and tile world passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
