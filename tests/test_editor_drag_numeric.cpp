// Typed values during a transform drag (artist worklist ART-029): type a number and a unit while
// moving, rotating or scaling; X/Y/Z change the axis; the value shows by the cursor; Enter commits
// one undo step and Esc restores the start.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near(float a, float b, float tolerance = 1.0e-3F) { return std::abs(a - b) <= tolerance; }
std::string text(Float3 v) { return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z); }

std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Typed");
    EditorObject block(1, "Block");
    block.voxelSizeMeters = 0.5F;
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) (void)block.voxels->set_voxel({x, y, z}, 1);
    document.add_object(std::move(block));
    auto c = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    c->resize(1600, 900);
    c->camera().position = {2.0F, 3.0F, 6.0F};
    c->camera().target = {0.5F, 0.5F, 0.5F};
    c->workspace().select_object(1);
    return c;
}

bool grab(NativeEditorController& c, int axisNumber) {
    for (const GizmoScreenAxis& axis : c.gizmo_axes())
        if (axis.axis == axisNumber) {
            const int x = static_cast<int>(axis.start.x + (axis.end.x - axis.start.x) * 0.8F);
            const int y = static_cast<int>(axis.start.y + (axis.end.y - axis.start.y) * 0.8F);
            c.pointer_move(x, y);
            c.pointer_down(PointerButton::Primary, x, y);
            c.pointer_move(x + 7, y + 3);
            return c.gizmo_dragging();
        }
    return false;
}

void type(NativeEditorController& c, std::string_view keys) {
    for (const char k : keys) c.key_down(std::string(1, k), false, false, false);
}

Float3 position(NativeEditorController& c) { return c.workspace().document().find_object(1)->transform.position; }

void test_move_with_units() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.set_active_tool(EditorToolId::Translate);
    check(grab(c, 1), "grab Move X");
    type(c, "25cm");
    check(near(position(c).x, 0.25F) && near(position(c).y, 0.0F), "25cm moves 0.25 m along X: " + text(position(c)));
    check(c.transform_numeric_label().rfind("Move X: 25cm = 0.25 m", 0) == 0, "the label: " + c.transform_numeric_label());
    struct Canvas final : IEditorCanvas {
        void fill(UiRect, EditorColor) const override {}
        void outline(UiRect, EditorColor) const override {}
        void line(int, int, int, int, EditorColor, int) const override {}
        void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
        [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
        mutable std::vector<std::string> texts;
    } canvas;
    render_native_editor(canvas, c, 1600, 900);
    bool drawn = false;
    for (const std::string& t : canvas.texts) drawn = drawn || t.rfind("Move X: 25cm", 0) == 0;
    check(drawn, "it is drawn by the cursor");
    c.key_down("y", false, false, false);
    check(near(position(c).x, 0.0F) && near(position(c).y, 0.25F), "Y switches the axis: " + text(position(c)));
    c.key_down("backspace", false, false, false);
    c.key_down("backspace", false, false, false);
    check(c.transform_numeric_text() == "25", "backspace edits the text");
    check(near(position(c).y, 25.0F), "25 alone is meters");
    c.key_down("backspace", false, false, false);
    c.key_down("backspace", false, false, false);
    type(c, "-1.5ft");
    check(near(position(c).y, -0.4572F), "-1.5ft: " + text(position(c)));
    const std::size_t steps = c.workspace().commands().size();
    c.key_down("enter", false, false, false);
    check(!c.gizmo_dragging() && near(position(c).y, -0.4572F) && c.workspace().commands().size() == steps + 1, "Enter commits one step");
    // Esc restores.
    check(grab(c, 3), "grab Move Z");
    type(c, "2");
    check(near(position(c).z, 2.0F), "typed 2 m on Z");
    c.key_down("escape", false, false, false);
    check(near(position(c).z, 0.0F) && near(position(c).y, -0.4572F), "Esc restores the start");
}

void test_rotate_and_scale() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.set_active_tool(EditorToolId::Rotate);
    check(grab(c, 2), "grab Rotate Y");
    type(c, "90");
    const Quaternion r = c.workspace().document().find_object(1)->transform.rotation;
    const Float3 forward = rotate(r, {1.0F, 0.0F, 0.0F});
    check(near(forward.z, -1.0F, 0.01F), "90 turns a quarter about Y: " + text(forward));
    check(c.transform_numeric_label().rfind("Rotate Y: 90", 0) == 0, "rotate label: " + c.transform_numeric_label());
    c.key_down("backspace", false, false, false);
    c.key_down("backspace", false, false, false);
    type(c, "0.5turn");
    const Float3 half = rotate(c.workspace().document().find_object(1)->transform.rotation, {1.0F, 0.0F, 0.0F});
    check(near(half.x, -1.0F, 0.01F), "0.5turn is half a turn: " + text(half));
    c.key_down("escape", false, false, false);

    c.set_active_tool(EditorToolId::Scale);
    check(grab(c, 1), "grab Scale X");
    type(c, "2");
    check(near(c.workspace().document().find_object(1)->voxelScale.x, 2.0F), "typed scale 2 on X");
    c.key_down("z", false, false, false);
    const Float3 s = c.workspace().document().find_object(1)->voxelScale;
    check(near(s.x, 1.0F) && near(s.z, 2.0F), "Z moves the typed scale to Z: " + text(s));
    type(c, "cm");
    check(c.transform_numeric_text() == "2", "no units for a scale factor");
    c.key_down("enter", false, false, false);
    check(near(c.workspace().document().find_object(1)->voxelScale.z, 2.0F), "committed");
}
}  // namespace

int main() {
    test_move_with_units();
    test_rotate_and_scale();
    if (g_failures == 0) std::printf("drag numeric tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
