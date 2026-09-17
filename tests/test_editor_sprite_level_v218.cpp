#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

namespace {
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class CountingCanvas final : public IEditorCanvas {
public:
    void fill(UiRect, EditorColor) const override { ++fills; }
    void outline(UiRect, EditorColor) const override { ++outlines; }
    void line(int, int, int, int, EditorColor, int) const override { ++lines; }
    void text(int, int, std::string_view value, EditorColor) const override {
        ++texts;
        if (value.find("SPRITE LEVEL") != std::string_view::npos) sawLevelLabel = true;
        if (value.find("Sprite Animation State Graph") != std::string_view::npos) sawGraphLabel = true;
    }
    [[nodiscard]] int text_width(std::string_view value) const override {
        return static_cast<int>(value.size()) * 7;
    }
    mutable int fills{};
    mutable int outlines{};
    mutable int lines{};
    mutable int texts{};
    mutable bool sawLevelLabel{};
    mutable bool sawGraphLabel{};
};
}

int main() {
    using namespace dve::editor;
    try {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.configure_ai_assistant(DVE_SOURCE_DIR);
        controller.resize(1280, 800);
        require(controller.workspace().menus().find("sprite.play_level") != nullptr,
                "Play Original Sprite Level is missing from the Sprite menu");
        require(controller.dispatch_action("sprite.play_level"), "sprite level action failed");
        require(controller.sprite_level_playing() && controller.sprite_level() &&
                    controller.sprite_level_presentation(),
                "sprite level did not initialize in the editor");
        controller.key_down("d", false, false, false);
        for (int frame = 0; frame < 20; ++frame) controller.update(1.0F / 60.0F);
        controller.key_up("d", false, false, false);
        controller.key_down("space", false, false, false);
        controller.update(1.0F / 60.0F);
        controller.key_up("space", false, false, false);
        controller.key_down("x", false, false, false);
        controller.update(1.0F / 60.0F);
        require(controller.sprite_level()->state().frame >= 22U,
                "editor level input did not advance the deterministic simulation");
        const auto frame = controller.sprite_level_presentation()->build_frame(*controller.sprite_level());
        require(!frame.sprites.items.empty() && !frame.tiles.empty(),
                "editor level preview has no sprite or tile submissions");
        CountingCanvas canvas;
        render_native_editor(canvas, controller, 1280, 800);
        require(canvas.fills > 20 && canvas.texts > 10 && canvas.sawLevelLabel,
                "editor canvas did not render the sprite level preview");
        controller.key_down("escape", false, false, false);
        require(!controller.sprite_level_playing(), "Escape did not stop the editor sprite level");
        require(controller.workspace().menus().find("sprite.graph") != nullptr,
                "Animation State Graph is missing from the Sprite menu");
        require(controller.dispatch_action("sprite.graph"), "sprite graph action failed");
        require(controller.sprite_animation_graph_open(), "sprite graph did not open");
        controller.key_down("c", false, false, false);
        require(!controller.sprite_animation_graph().comments().empty(),
                "graph comment keyboard action failed");
        CountingCanvas graphCanvas;
        render_native_editor(graphCanvas, controller, 1280, 800);
        require(graphCanvas.sawGraphLabel && graphCanvas.lines > 5 && graphCanvas.texts > 10,
                "native editor did not render the polished sprite graph panel");
        controller.key_down("escape", false, false, false);
        require(!controller.sprite_animation_graph_open(), "Escape did not close sprite graph");
        std::cout << "v2.18 editor sprite level and graph passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
