#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct RecordingCanvas final : IEditorCanvas {
    mutable int fills{};
    mutable int lines{};
    mutable int texts{};
    mutable bool sawPixel{};
    mutable bool sawRig{};
    void fill(UiRect, EditorColor) const override { ++fills; }
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override { ++lines; }
    void text(int, int, std::string_view value, EditorColor) const override {
        ++texts;
        if (value.find("Pixel Art Studio") != std::string_view::npos) sawPixel = true;
        if (value.find("Multi-Part Sprite Rig") != std::string_view::npos) sawRig = true;
    }
    [[nodiscard]] int text_width(std::string_view value) const override {
        return static_cast<int>(value.size()) * 8;
    }
};

void run() {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(1280, 800);
    require(controller.workspace().menus().find("sprite.pixel_art") != nullptr,
            "Pixel Art Studio action is missing");
    require(controller.workspace().menus().find("sprite.rig2d") != nullptr,
            "Multi-Part Sprite Rig action is missing");

    require(controller.dispatch_action("sprite.pixel_art"), "could not open Pixel Art Studio");
    require(controller.sprite_pixel_art_open(), "Pixel Art Studio did not open");
    const std::uint64_t before = controller.sprite_pixel_art().document().contentHash;
    const UiRect pixelCanvas = controller.sprite_pixel_art_canvas();
    controller.pointer_down(PointerButton::Primary,
                            pixelCanvas.x + pixelCanvas.width / 2,
                            pixelCanvas.y + pixelCanvas.height / 2);
    controller.pointer_up(PointerButton::Primary,
                          pixelCanvas.x + pixelCanvas.width / 2,
                          pixelCanvas.y + pixelCanvas.height / 2);
    require(controller.sprite_pixel_art().document().contentHash != before,
            "native Pixel Art Studio pointer input did not paint");
    controller.key_down("n", false, false, false);
    controller.key_down("l", false, false, false);
    require(controller.sprite_pixel_art().document().frames.size() == 2U,
            "native Pixel Art Studio did not add a frame");
    require(controller.sprite_pixel_art().document().frames.back().layers.size() == 2U,
            "native Pixel Art Studio did not add a layer");
    RecordingCanvas pixelRenderer;
    render_native_editor(pixelRenderer, controller, 1280, 800);
    require(pixelRenderer.sawPixel && pixelRenderer.fills > 20,
            "native Pixel Art Studio was not rendered");

    require(controller.dispatch_action("sprite.rig2d"), "could not open Multi-Part Sprite Rig");
    require(controller.sprite_rig2d_open() && !controller.sprite_pixel_art_open(),
            "opening the sprite rig did not switch top-level authoring surfaces");
    const std::size_t bonesBefore = controller.sprite_rig2d().asset().bones.size();
    controller.key_down("b", false, false, false);
    controller.key_down("p", false, false, false);
    require(controller.sprite_rig2d().asset().bones.size() == bonesBefore + 1U,
            "native rig panel did not add a bone");
    require(controller.sprite_rig2d().asset().parts.size() >= 2U,
            "native rig panel did not add a sprite part");
    controller.update(0.25F);
    RecordingCanvas rigRenderer;
    render_native_editor(rigRenderer, controller, 1280, 800);
    require(rigRenderer.sawRig && rigRenderer.lines > 0 && rigRenderer.texts > 8,
            "native Multi-Part Sprite Rig was not rendered");
}
}

int main() {
    try {
        run();
        std::cout << "dve_editor_sprite_pixel_rig_v222_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
