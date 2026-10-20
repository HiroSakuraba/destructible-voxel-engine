// Real scale (artist worklist ART-027): the Scale gizmo (axis handles and the uniform centre
// square) and the inspector Scale field stretch geometry without resampling, as one undoable
// transform; Voxel > Resample Voxels to Scale is the separate step that rebuilds the voxels.
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
bool near(float a, float b, float tolerance = 1.0e-4F) { return std::abs(a - b) <= tolerance; }
std::string text(Float3 v) { return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z); }

// "Block": 4 x 4 x 4 voxels of 0.25 m. "Mesh": a unit cube. "Label": 3D text.
std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Scale");
    EditorObject block(1, "Block");
    block.voxelSizeMeters = 0.25F;
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y)
            for (int z = 0; z < 4; ++z) (void)block.voxels->set_voxel({x, y, z}, 1);
    document.add_object(std::move(block));
    EditorObject mesh(2, "Mesh");
    mesh.polygon = make_editor_polygon_cube(2);
    mesh.transform.position = {3.0F, 0.0F, 0.0F};
    document.add_object(std::move(mesh));
    auto c = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    c->resize(1600, 900);
    c->camera().position = {1.5F, 2.5F, 7.0F};
    c->camera().target = {1.5F, 0.5F, 0.0F};
    c->workspace().select_object(1);
    return c;
}

Float3 scale_of(NativeEditorController& c, EditorObjectId id) { return c.workspace().document().find_object(id)->voxelScale; }
std::uint64_t cells(NativeEditorController& c) { return c.workspace().document().find_object(1)->voxels->occupied_voxel_count(); }

void test_gizmo() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.set_active_tool(EditorToolId::Scale);
    const auto axes = c.gizmo_axes();
    check(axes.size() == 3, "scale gizmo");
    if (axes.size() != 3) return;
    const int cx = static_cast<int>(axes[0].start.x), cy = static_cast<int>(axes[0].start.y);
    check(c.hit_test_gizmo_axis(cx + 2, cy - 1) == 4, "the centre square is the uniform handle");
    c.pointer_move(cx + 2, cy - 1);
    c.pointer_down(PointerButton::Primary, cx + 2, cy - 1);
    c.pointer_move(cx + 60, cy - 60);
    c.pointer_up(PointerButton::Primary, cx + 60, cy - 60);
    const Float3 uniform = scale_of(c, 1);
    check(uniform.x > 1.2F && near(uniform.x, uniform.y) && near(uniform.y, uniform.z), "uniform scale from the centre: " + text(uniform));
    check(cells(c) == 64, "voxels kept");
    check(c.dispatch_action("edit.undo") && near(scale_of(c, 1).x, 1.0F), "one undo");

    // The X handle scales X only.
    const auto x = c.gizmo_axes()[0];
    const int hx = static_cast<int>(x.end.x), hy = static_cast<int>(x.end.y);
    c.pointer_move(hx, hy);
    c.pointer_down(PointerButton::Primary, hx, hy);
    c.pointer_move(hx + static_cast<int>((x.end.x - x.start.x) * 0.8F), hy + static_cast<int>((x.end.y - x.start.y) * 0.8F));
    c.pointer_up(PointerButton::Primary, hx + static_cast<int>((x.end.x - x.start.x) * 0.8F), hy + static_cast<int>((x.end.y - x.start.y) * 0.8F));
    const Float3 axis = scale_of(c, 1);
    check(axis.x > 1.2F && near(axis.y, 1.0F) && near(axis.z, 1.0F), "the X handle stretches X only: " + text(axis));
    check(cells(c) == 64, "still the same voxels");
}

void test_inspector_field() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    check(c.number_field_available(NumberFieldId::Scale), "the inspector shows Scale for a voxel object");
    check(c.number_field_text(NumberFieldId::Scale, 0) == "1", "Scale X reads 1: " + c.number_field_text(NumberFieldId::Scale, 0));
    check(c.set_number_field(NumberFieldId::Scale, 1, "2") && near(scale_of(c, 1).y, 2.0F) && cells(c) == 64, "type Scale Y = 2");
    check(c.set_number_field(NumberFieldId::Scale, 1, "*=1.5") && near(scale_of(c, 1).y, 3.0F), "relative edits work");
    check(c.set_number_field(NumberFieldId::Scale, 0, "0.5 1 2") && near(scale_of(c, 1).x, 0.5F) && near(scale_of(c, 1).y, 1.0F) &&
          near(scale_of(c, 1).z, 2.0F), "three values set X, Y and Z");
    check(!c.set_number_field(NumberFieldId::Scale, 0, "0"), "zero is refused");
    check(!c.set_number_field(NumberFieldId::Scale, 0, "100") && c.status().text.find("1 mm and 10 m") != std::string::npos,
          "a 25 m cell is refused: " + c.status().text);
    check(c.dispatch_action("edit.undo") && near(scale_of(c, 1).y, 3.0F), "undo the triple");
    check(c.reset_number_field(NumberFieldId::Scale, 1) && near(scale_of(c, 1).y, 1.0F), "reset to 1");

    // Mesh too; text objects have no Scale row.
    c.workspace().select_object(2);
    check(c.number_field_available(NumberFieldId::Scale) && c.set_number_field(NumberFieldId::Scale, 2, "3") &&
          near(scale_of(c, 2).z, 3.0F), "meshes stretch the same way");

    // Drawn in the inspector, with the × suffix.
    struct Canvas final : IEditorCanvas {
        void fill(UiRect, EditorColor) const override {}
        void outline(UiRect, EditorColor) const override {}
        void line(int, int, int, int, EditorColor, int) const override {}
        void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
        [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
        mutable std::vector<std::string> texts;
    } canvas;
    render_native_editor(canvas, c, 1600, 900);
    bool label = false, value = false;
    for (const std::string& t : canvas.texts) {
        label = label || t == "Scale";
        value = value || t == "3\xC3\x97";
    }
    check(label && value, "the Scale row is drawn");

    // Scrub the X letter.
    c.workspace().select_object(1);
    std::optional<NumberFieldRect> rect;
    for (const NumberFieldRect& r : c.number_field_rects()) if (r.field == NumberFieldId::Scale && r.axis == 0) rect = r;
    check(rect.has_value(), "Scale X has a field");
    if (rect) {
        const int x = rect->label.x + 4, y = rect->label.y + 8;
        const std::size_t steps = c.workspace().commands().size();
        c.pointer_move(x, y);
        c.pointer_down(PointerButton::Primary, x, y);
        c.pointer_move(x + 40, y);
        c.pointer_up(PointerButton::Primary, x + 40, y);
        check(scale_of(c, 1).x > 1.3F && cells(c) == 64, "scrubbing stretches X: " + text(scale_of(c, 1)));
        check(c.workspace().commands().size() == steps + 1, "one undo step");
    }
}

void test_scale_field_precision_and_reset_row() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    check(c.set_number_field(NumberFieldId::Scale, 0, "1.5"), "set Scale X 1.5");
    check(c.number_field_text(NumberFieldId::Scale, 0) == "1.5", "fractional scale is shown as typed: " + c.number_field_text(NumberFieldId::Scale, 0));
    const Transform before = c.workspace().document().find_object(1)->transform;
    for (const NumberFieldRect& r : c.number_field_rects())
        if (r.field == NumberFieldId::Scale && r.axis == 1) {
            c.pointer_move(r.box.x + 4, r.box.y + 8);
            c.pointer_down(PointerButton::Secondary, r.box.x + 4, r.box.y + 8);
            c.pointer_up(PointerButton::Secondary, r.box.x + 4, r.box.y + 8);
            break;
        }
    const std::size_t steps = c.workspace().commands().size();
    check(c.dispatch_action("inspector.field.reset_row"), "Reset Scale X, Y, Z: " + c.status().text);
    check(near(scale_of(c, 1).x, 1.0F) && c.workspace().commands().size() == steps + 1, "the row resets the scale in one step");
    const Transform after = c.workspace().document().find_object(1)->transform;
    check(after.rotation.x == before.rotation.x && after.rotation.y == before.rotation.y && after.rotation.z == before.rotation.z &&
          after.rotation.w == before.rotation.w, "rotation is untouched");
    check(c.dispatch_action("inspector.field.reset_row") && c.workspace().commands().size() == steps + 1, "a second reset is a no-op");
}

void test_resample() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    check(!c.dispatch_action("voxel.resample_to_scale"), "nothing to resample at scale 1");
    const auto menu_disabled = [&] {
        const MenuAction* action = std::as_const(c.workspace().menus()).find("voxel.resample_to_scale");
        return action && !action->enabled && !action->disabledReason.empty();
    };
    check(menu_disabled(), "the menu entry is disabled with a reason at scale 1");
    (void)c.set_number_field(NumberFieldId::Scale, 0, "2");
    {
        const std::size_t steps = c.workspace().commands().size();
        c.workspace().document().find_object(1)->flags.locked = true;
        check(!c.dispatch_action("voxel.resample_to_scale") && cells(c) == 64 && c.workspace().commands().size() == steps,
              "a locked object is refused without an undo step");
        check(menu_disabled(), "and the menu says why");
        c.workspace().document().find_object(1)->flags.locked = false;
    }
    const EditorObjectBounds before = object_world_bounds(*c.workspace().document().find_object(1));
    check(c.dispatch_action("voxel.resample_to_scale"), "Resample Voxels to Scale: " + c.status().text);
    check(near(scale_of(c, 1).x, 1.0F) && cells(c) == 128, "cells rebuilt and Scale back to 1: " + std::to_string(cells(c)));
    const EditorObjectBounds after = object_world_bounds(*c.workspace().document().find_object(1));
    check(near(before.minimum.x, after.minimum.x, 0.01F) && near(before.maximum.x, after.maximum.x, 0.01F) &&
          near(before.maximum.y, after.maximum.y, 0.01F), "the object keeps its place and size: " + text(before.maximum) + " / " + text(after.maximum));
    check(c.dispatch_action("edit.undo") && near(scale_of(c, 1).x, 2.0F) && cells(c) == 64, "one undo restores both");
}
}  // namespace

int main() {
    test_gizmo();
    test_inspector_field();
    test_scale_field_precision_and_reset_row();
    test_resample();
    if (g_failures == 0) std::printf("real scale tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
