// Numeric fields (artist worklist ART-084 / ART-085):
//   - values with units and arithmetic, relative += -= *= /=;
//   - per-axis Position / Rotation boxes that set every selected object, "—" when they differ;
//   - Voxel size; Tab / Shift+Tab between boxes;
//   - scrubbing by dragging an axis letter (live, one undo, Esc cancels, Shift fine, Ctrl snap);
//   - right-click Reset / Reset row / Copy / Paste;
//   - the viewport tool strip: Brush radius (voxel brush tools) and Surface offset (Select/Move).
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_number_field.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near(double a, double b, double tolerance = 1.0e-4) { return std::abs(a - b) <= tolerance; }
const std::string kDash = "\xE2\x80\x94";

void test_parser() {
    const auto m = [](std::string_view text, double current = 0.0) { return parse_number_field(text, NumberFieldUnit::Meters, current); };
    const auto d = [](std::string_view text, double current = 0.0) { return parse_number_field(text, NumberFieldUnit::Degrees, current); };
    check(m("1.5").ok && near(m("1.5").value, 1.5), "plain meters");
    check(near(m("150cm").value, 1.5) && near(m("150 cm").value, 1.5) && near(m("25mm").value, 0.025), "cm and mm");
    check(near(m("2 ft").value, 0.6096) && near(m("6in").value, 0.1524) && near(m("1'6\"").value, 0.4572), "feet and inches");
    check(near(m("2*0.25").value, 0.5) && near(m("(1+2)/4").value, 0.75) && near(m("-0.5").value, -0.5) && near(m("1e-2").value, 0.01),
          "arithmetic and exponents");
    check(near(m("1m + 50cm").value, 1.5), "mixed units add");
    check(near(m("+=0.5", 2.0).value, 2.5) && near(m("-=10cm", 1.0).value, 0.9) && near(m("*=2", 3.0).value, 6.0) &&
              near(m("/=4", 2.0).value, 0.5),
          "relative edits");
    check(near(d("90").value, 90.0) && near(d("0.25turn").value, 90.0) && near(d("(pi/2)rad").value, 90.0) && near(d("45deg").value, 45.0),
          "angles");
    check(!m("10 deg").ok && m("10 deg").error.find("Angles") != std::string::npos, "an angle in a length field is refused");
    check(!d("2m").ok, "a length in an angle field is refused");
    check(!m("abc").ok && !m("").ok && !m("1/0").ok && !m("nan").ok && !m("1 2").ok && !m("2 furlongs").ok, "garbage is refused");
    check(!parse_number_field("3cm", NumberFieldUnit::Count, 0).ok && parse_number_field("3", NumberFieldUnit::Count, 0).ok, "counts have no units");
    check(format_number_field(1.5, NumberFieldUnit::Meters) == "1.5" && format_number_field(-0.00001, NumberFieldUnit::Meters) == "0" &&
              format_number_field(90.0, NumberFieldUnit::Degrees) == "90" && format_number_field(0.12345, NumberFieldUnit::Meters) == "0.1235",
          "compact formatting");
}

EditorDocument make_scene() {
    EditorDocument document("Fields");
    for (EditorObjectId id = 1; id <= 2; ++id) {
        EditorObject object(id, id == 1 ? "Crate" : "Barrel");
        object.voxelSizeMeters = 0.25F;
        object.transform.position = {id == 1 ? 0.0F : 4.0F, 1.0F, 0.0F};
        for (int x = 0; x < 2; ++x) (void)object.voxels->set_voxel({x, 0, 0}, 1);
        document.add_object(std::move(object));
    }
    document.add_object(EditorObject(3, "Empty"));
    return document;
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    controller->resize(1280, 720);
    controller->workspace().select_object(1);
    return controller;
}

std::optional<NumberFieldRect> find_rect(const NativeEditorController& c, NumberFieldId field, int axis = 0) {
    for (const NumberFieldRect& rect : c.number_field_rects())
        if (rect.field == field && rect.axis == axis) return rect;
    return std::nullopt;
}

void click(NativeEditorController& c, UiRect r, PointerButton button = PointerButton::Primary) {
    const int x = r.x + r.width / 2, y = r.y + r.height / 2;
    c.pointer_move(x, y);
    c.pointer_down(button, x, y);
    c.pointer_up(button, x, y);
}

void type(NativeEditorController& c, const char* value, const char* key = "enter", bool shift = false) {
    c.text_input(value);
    c.key_down(key, false, shift, false);
}

const EditorObject& object(NativeEditorController& c, EditorObjectId id) { return *c.workspace().document().find_object(id); }

struct Canvas final : IEditorCanvas {
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    mutable std::vector<std::string> texts;
};

void test_typing_per_axis() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    const auto y = find_rect(c, NumberFieldId::Position, 1);
    check(y.has_value() && find_rect(c, NumberFieldId::Rotation, 2).has_value(), "Position and Rotation have X/Y/Z boxes");
    if (!y) return;
    click(c, y->box);
    check(c.text_edit().kind == TextEditKind::Position && c.text_edit().axis == 1 && c.text_edit().buffer == "1",
          "clicking the Y box types into Y, starting from its value");
    type(c, "150cm");
    check(near(object(c, 1).transform.position.y, 1.5) && near(object(c, 1).transform.position.x, 0.0), "Y = 150 cm; X untouched");
    check(c.workspace().commands().can_undo() && std::string(c.workspace().commands().undo_label()).find("Position Y") != std::string::npos,
          "one undo step named for the axis");

    // Two objects: X differs (shown as a dash), Z is shared.
    c.workspace().add_to_selection(2);
    check(c.number_field_text(NumberFieldId::Position, 0) == kDash && c.number_field_text(NumberFieldId::Position, 2) == "0",
          "differing values read as a dash");
    Canvas canvas;
    render_native_editor(canvas, c, 1280, 720);
    check(canvas.has(kDash) && canvas.has("Edits set each object") && canvas.has("X") && canvas.has("Position"),
          "the inspector shows the dash and the scope");
    click(c, find_rect(c, NumberFieldId::Position, 0)->box);
    check(c.text_edit().buffer.empty(), "a differing field starts empty");
    type(c, "5");
    check(near(object(c, 1).transform.position.x, 5.0) && near(object(c, 2).transform.position.x, 5.0) &&
              near(object(c, 2).transform.position.y, 1.0),
          "X = 5 sets both; their Y is untouched");
    check(c.workspace().commands().undo(c.workspace().document()).success && near(object(c, 2).transform.position.x, 4.0),
          "one undo restores both");
    click(c, find_rect(c, NumberFieldId::Position, 0)->box);
    type(c, "+=2");
    check(near(object(c, 1).transform.position.x, 2.0) && near(object(c, 2).transform.position.x, 6.0), "+= offsets each one");

    // Rotation, Tab and Shift+Tab.
    c.workspace().select_object(1);
    click(c, find_rect(c, NumberFieldId::Rotation, 2)->box);
    type(c, "(pi/2)rad", "tab");
    check(c.text_edit().kind == TextEditKind::VoxelSize, "Tab from Rotation Z moves to Voxel size");
    const Float3 euler = multiply(quaternion_to_euler_xyz(object(c, 1).transform.rotation), 57.29578F);
    check(near(euler.z, 90.0, 0.01), "Rotation Z = pi/2 rad is 90 degrees");
    c.key_down("tab", false, true, false);
    check(c.text_edit().kind == TextEditKind::Rotation && c.text_edit().axis == 2, "Shift+Tab goes back");
    c.key_down("escape", false, false, false);
    check(c.text_edit().kind == TextEditKind::Inactive, "Esc closes the field");

    // Refusals.
    click(c, find_rect(c, NumberFieldId::Position, 0)->box);
    type(c, "10 deg");
    check(c.status().error && c.status().text.find("Angles are not allowed") != std::string::npos, "a wrong unit says why");
    c.workspace().document().find_object(1)->flags.locked = true;
    click(c, find_rect(c, NumberFieldId::Position, 0)->box);
    type(c, "3");
    check(c.status().text.find("Unlock") != std::string::npos && near(object(c, 1).transform.position.x, 2.0), "a locked object is refused");
}

void test_voxel_size() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    const auto size = find_rect(c, NumberFieldId::VoxelSize);
    check(size.has_value() && c.number_field_text(NumberFieldId::VoxelSize, 0) == "0.25", "Voxel size shows for a voxel object");
    if (!size) return;
    click(c, size->box);
    type(c, "5cm");
    check(near(object(c, 1).voxelSizeMeters, 0.05), "voxel size typed in cm");
    check(c.workspace().commands().undo(c.workspace().document()).success && near(object(c, 1).voxelSizeMeters, 0.25), "one undo");
    click(c, size->box);
    type(c, "0");
    check(c.status().error && c.status().text.find("between 1 mm and 10 m") != std::string::npos && near(object(c, 1).voxelSizeMeters, 0.25),
          "an out-of-range size is refused");
    c.workspace().select_object(3);
    check(!find_rect(c, NumberFieldId::VoxelSize).has_value(), "an object without voxels has no Voxel size field");
}

void test_scrub() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    const auto x = find_rect(c, NumberFieldId::Position, 0);
    if (!x) { check(false, "X box"); return; }
    const int lx = x->label.x + 5, ly = x->label.y + 9;
    c.pointer_move(lx, ly);
    c.pointer_down(PointerButton::Primary, lx, ly);
    c.pointer_move(lx + 50, ly);
    check(c.number_scrub_active() && near(object(c, 1).transform.position.x, 0.5, 1e-3), "dragging the X letter scrubs live (0.01 m/px)");
    check(!c.workspace().commands().can_undo(), "no undo step while scrubbing");
    c.pointer_up(PointerButton::Primary, lx + 50, ly);
    check(near(object(c, 1).transform.position.x, 0.5, 1e-3) && c.workspace().commands().can_undo(), "release applies it as one undo step");
    check(c.workspace().commands().undo(c.workspace().document()).success && near(object(c, 1).transform.position.x, 0.0), "undo");

    c.pointer_down(PointerButton::Primary, lx, ly);
    c.pointer_move(lx + 50, ly, 1U);  // Shift: fine
    check(near(object(c, 1).transform.position.x, 0.05, 1e-3), "Shift scrubs ten times finer");
    c.pointer_move(lx + 37, ly, 2U);  // Ctrl: snap to the move step (0.1 m)
    check(near(object(c, 1).transform.position.x, 0.4, 1e-3), "Ctrl snaps to the move step");
    c.key_down("escape", false, false, false);
    check(!c.number_scrub_active() && near(object(c, 1).transform.position.x, 0.0) && !c.workspace().commands().can_undo(),
          "Esc cancels the scrub");
    c.pointer_up(PointerButton::Primary, lx + 37, ly);

    // Pressing the letter without dragging types into that axis.
    c.pointer_down(PointerButton::Primary, lx, ly);
    c.pointer_up(PointerButton::Primary, lx, ly);
    check(c.text_edit().kind == TextEditKind::Position && c.text_edit().axis == 0, "a click on the letter types");
    c.key_down("escape", false, false, false);

    // Voxel size scrubs multiplicatively, live and as one undo.
    const auto size = find_rect(c, NumberFieldId::VoxelSize);
    if (size) {
        c.pointer_down(PointerButton::Primary, size->label.x + 5, size->label.y + 9);
        c.pointer_move(size->label.x + 75, size->label.y + 9);
        const float live = object(c, 1).voxelSizeMeters;
        c.pointer_up(PointerButton::Primary, size->label.x + 75, size->label.y + 9);
        check(live > 0.4F && near(object(c, 1).voxelSizeMeters, live), "voxel size scrubs up and applies");
        check(c.workspace().commands().undo(c.workspace().document()).success && near(object(c, 1).voxelSizeMeters, 0.25), "undo");
    }
}

void test_context_menu() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().document().find_object(1)->transform.position = {3.0F, 2.0F, 1.0F};
    click(c, find_rect(c, NumberFieldId::Position, 0)->box, PointerButton::Secondary);
    check(c.context_menu().open && !c.context_menu().items.empty() && c.context_menu().items.front().label == "Reset X (0)",
          "right-click a field offers Reset");
    check(c.dispatch_action("inspector.field.reset") && near(object(c, 1).transform.position.x, 0.0) && near(object(c, 1).transform.position.y, 2.0),
          "Reset X");
    click(c, find_rect(c, NumberFieldId::Position, 1)->box, PointerButton::Secondary);
    check(c.dispatch_action("inspector.field.copy"), "Copy Y");
    click(c, find_rect(c, NumberFieldId::Position, 2)->box, PointerButton::Secondary);
    bool hasPaste = false;
    for (const auto& item : c.context_menu().items) hasPaste = hasPaste || item.label == "Paste 2";
    check(hasPaste && c.dispatch_action("inspector.field.paste") && near(object(c, 1).transform.position.z, 2.0), "Paste onto Z");
    check(c.dispatch_action("inspector.field.reset_row") && near(object(c, 1).transform.position.y, 0.0) &&
              near(object(c, 1).transform.position.z, 0.0),
          "Reset Position X, Y, Z");
    check(c.workspace().commands().undo(c.workspace().document()).success && near(object(c, 1).transform.position.z, 2.0),
          "the row reset is one undo step");
}

void test_tool_strip() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.set_active_tool(EditorToolId::AddVoxel);
    const auto radius = find_rect(c, NumberFieldId::BrushRadius);
    check(radius.has_value() && !find_rect(c, NumberFieldId::SurfaceOffset).has_value(), "Add shows Brush radius in the tool strip");
    if (!radius) return;
    check(c.layout().viewport.contains(radius->box.x, radius->box.y), "the strip is in the viewport");
    Canvas canvas;
    render_native_editor(canvas, c, 1280, 720);
    check(canvas.has("Brush radius"), "it is drawn");
    click(c, radius->box);
    type(c, "5");
    check(c.number_field_text(NumberFieldId::BrushRadius, 0) == "5", "typed radius");
    click(c, radius->box);
    type(c, "40");
    check(c.number_field_text(NumberFieldId::BrushRadius, 0) == "32" && c.status().text.find("most is 32") != std::string::npos, "the radius is capped at 32");
    c.pointer_down(PointerButton::Primary, radius->label.x + 5, radius->label.y + 9);
    c.pointer_move(radius->label.x + 5 - 80, radius->label.y + 9);
    c.pointer_up(PointerButton::Primary, radius->label.x + 5 - 80, radius->label.y + 9);
    check(c.number_field_text(NumberFieldId::BrushRadius, 0) == "22", "scrubbing the label changes the radius (8 px per voxel): " + c.number_field_text(NumberFieldId::BrushRadius, 0));

    c.set_active_tool(EditorToolId::Select);
    const auto offset = find_rect(c, NumberFieldId::SurfaceOffset);
    check(offset.has_value() && !find_rect(c, NumberFieldId::BrushRadius).has_value(), "Select shows Surface offset");
    if (offset) {
        click(c, offset->box);
        type(c, "10cm");
        check(c.number_field_text(NumberFieldId::SurfaceOffset, 0) == "0.1", "offset typed in cm");
        click(c, offset->box, PointerButton::Secondary);
        check(c.dispatch_action("inspector.field.reset") && c.number_field_text(NumberFieldId::SurfaceOffset, 0) == "0", "Reset offset");
    }
}
}  // namespace

int main() {
    test_parser();
    test_typing_per_axis();
    test_voxel_size();
    test_scrub();
    test_context_menu();
    test_tool_strip();
    if (g_failures == 0) std::printf("number field tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
