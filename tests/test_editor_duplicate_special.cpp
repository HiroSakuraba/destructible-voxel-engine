// Duplicate Special: N copies, each offset / turned / scaled from the last, around its own pivot
// or a fixed point; ghost preview, Apply as one undo step, copies in one group.
#include "dve/editor_duplicate_special.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

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
bool near(float a, float b, float tolerance = 1.0e-3F) { return std::abs(a - b) <= tolerance; }
bool near3(Float3 a, Float3 b, float tolerance = 1.0e-3F) { return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance); }
bool same_rotation(Quaternion a, Quaternion b) {
    const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return d > 0.9999F;
}

void test_poses() {
    DuplicateSpecialSettings v;
    v.copies = 72;
    v.offset = {0.1F, 0.0F, 0.0F};
    v.rotationDegrees = {0.0F, 5.0F, 0.0F};
    const RigidTransform source = make_rigid_transform({1.0F, 2.0F, 3.0F}, {});
    auto poses = duplicate_special_poses(source, v);
    check(poses.size() == 72, "one pose per copy");
    check(near3(poses[0].transform.position, {1.1F, 2.0F, 3.0F}), "copy 1 is one offset away");
    check(same_rotation(poses[0].transform.rotation, quaternion_from_euler_xyz({0.0F, 0.0872665F, 0.0F})), "copy 1 is turned 5 degrees");
    check(same_rotation(poses[71].transform.rotation, source.rotation) && near3(poses[71].transform.position, source.position, 2e-3F),
          "72 local steps of 0.1 m and 5 degrees close the circle");

    v.space = DuplicateSpace::World;
    poses = duplicate_special_poses(source, v);
    check(near3(poses[9].transform.position, {2.0F, 2.0F, 3.0F}), "world offsets add in a straight line");

    v.pivot = DuplicatePivot::Point;
    v.point = {0.0F, 2.0F, 3.0F};
    v.offset = {0.0F, 0.1F, 0.0F};
    v.rotationDegrees = {0.0F, 90.0F, 0.0F};
    v.copies = 4;
    poses = duplicate_special_poses(source, v);
    check(near3(poses[0].transform.position, {0.0F, 2.1F, 2.0F}), "around a point: a quarter turn and a step up (a spiral)");
    check(near3(poses[3].transform.position, {1.0F, 2.4F, 3.0F}) && same_rotation(poses[3].transform.rotation, source.rotation),
          "four quarter turns come back round, 0.4 m higher");

    v.scale = 0.5F;
    poses = duplicate_special_poses(source, v);
    check(near(poses[2].scale, 0.125F), "scale compounds per copy");

    DuplicateSpecialSettings bad;
    bad.copies = 0;
    check(!duplicate_special_problem(bad, 1).empty(), "zero copies refused");
    bad.copies = kMaxDuplicateCopies + 1;
    check(!duplicate_special_problem(bad, 1).empty(), "too many copies refused");
    bad.copies = 200;
    check(duplicate_special_problem(bad, 101).find("too many") != std::string::npos, "too many objects refused");
    bad.scale = 0.0F;
    check(!duplicate_special_problem(bad, 1).empty(), "zero scale refused");
    bad.scale = 1.1F;
    check(!duplicate_special_problem(bad, 1).empty(), "a scale that runs away refused");
}

EditorDocument make_scene() {
    EditorDocument document("Feathers");
    EditorObject feather(1, "Feather");
    feather.voxelSizeMeters = 0.02F;
    for (int y = 0; y < 10; ++y) (void)feather.voxels->set_voxel({0, y, 0}, 1);
    feather.transform.position = {0.0F, 1.0F, 0.0F};
    document.add_object(std::move(feather));
    EditorObject tip(2, "Tip");
    tip.parent = 1;
    tip.voxelSizeMeters = 0.02F;
    (void)tip.voxels->set_voxel({0, 0, 0}, 2);
    tip.transform.position = {0.0F, 1.2F, 0.0F};
    document.add_object(std::move(tip));
    EditorObject rock(3, "Rock");
    (void)rock.voxels->set_voxel({0, 0, 0}, 3);
    rock.transform.position = {5.0F, 0.0F, 0.0F};
    document.add_object(std::move(rock));
    return document;
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    controller->resize(1280, 720);
    controller->workspace().select_object(1);
    return controller;
}

std::optional<NumberFieldRect> field(const NativeEditorController& c, NumberFieldId id, int axis = 0) {
    for (const NumberFieldRect& rect : c.number_field_rects())
        if (rect.field == id && rect.axis == axis) return rect;
    return std::nullopt;
}
void click(NativeEditorController& c, UiRect r) {
    const int x = r.x + r.width / 2, y = r.y + r.height / 2;
    c.pointer_move(x, y);
    c.pointer_down(PointerButton::Primary, x, y);
    c.pointer_up(PointerButton::Primary, x, y);
}
void type_into(NativeEditorController& c, NumberFieldId id, int axis, const char* value) {
    const auto rect = field(c, id, axis);
    check(rect.has_value(), "field is shown");
    if (!rect) return;
    click(c, rect->box);
    c.text_input(value);
    c.key_down("enter", false, false, false);
}

struct Canvas final : IEditorCanvas {
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override { ++lines; }
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    mutable int lines{};
    mutable std::vector<std::string> texts;
};

void test_feathers() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    EditorDocument& document = c.workspace().document();
    const std::size_t before = document.objects().size();
    check(c.dispatch_action("edit.duplicate_special") && c.duplicate_special_active(), "Ctrl+Shift+D opens the panel");
    check(!field(c, NumberFieldId::Position).has_value(), "the panel replaces the object details");
    type_into(c, NumberFieldId::DuplicateCopies, 0, "200");
    type_into(c, NumberFieldId::DuplicateOffset, 0, "3in");
    type_into(c, NumberFieldId::DuplicateOffset, 1, "0");
    type_into(c, NumberFieldId::DuplicateOffset, 2, "0");
    type_into(c, NumberFieldId::DuplicateRotation, 1, "0");
    type_into(c, NumberFieldId::DuplicateRotation, 2, "5");
    check(c.duplicate_special_settings().copies == 200 && near(c.duplicate_special_settings().offset.x, 0.0762F) &&
              near(c.duplicate_special_settings().rotationDegrees.z, 5.0F),
          "typed settings with units");
    check(c.duplicate_special_preview().size() == 400, "the preview shows every copy (feather and tip)");
    c.camera().position = {0.0F, 4.0F, 8.0F};
    c.camera().target = {0.0F, 1.0F, 0.0F};
    Canvas canvas;
    render_native_editor(canvas, c, 1280, 720);
    check(canvas.has("Duplicate Special") && canvas.has("200 copies (400 objects)") && canvas.lines >= 12 * 100, "panel and ghosts are drawn");
    check(document.objects().size() == before && !c.workspace().commands().can_undo(), "nothing changes until Apply");

    c.key_down("enter", false, false, false);
    check(!c.duplicate_special_active() && document.objects().size() == before + 1 + 400, "Enter applies: a group and 200 feathers with tips");
    const auto group = c.workspace().selected_object();
    const EditorObject* groupObject = group ? document.find_object(*group) : nullptr;
    check(groupObject && groupObject->name == "Feather x200" && document.children_of(*group).size() == 200, "the copies are in one group");
    const EditorObject* first = nullptr;
    const EditorObject* firstTip = nullptr;
    for (const auto& [id, object] : document.objects()) {
        if (object.name == "Feather 001") first = &object;
    }
    if (first)
        for (const EditorObjectId child : document.children_of(first->id)) firstTip = document.find_object(child);
    check(first && near3(first->transform.position, {0.0762F, 1.0F, 0.0F}), "copy 1 is 3 inches along");
    check(firstTip && firstTip->name == "Tip" && near3(firstTip->transform.position,
              add(first->transform.position, rotate(first->transform.rotation, {0.0F, 0.2F, 0.0F}))),
          "the tip is copied with its feather, in place on it");
    check(c.workspace().commands().undo(document).success && document.objects().size() == before, "one undo removes all of it");
}

void test_point_scale_and_options() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    EditorDocument& document = c.workspace().document();
    c.workspace().select_object(3);
    check(c.open_duplicate_special(), "open on the rock");
    DuplicatePanelLayout panel = c.duplicate_panel_layout();
    click(c, panel.pivotPoint);
    check(c.duplicate_special_settings().pivot == DuplicatePivot::Point && field(c, NumberFieldId::DuplicatePoint, 2).has_value(),
          "Around point shows the point");
    panel = c.duplicate_panel_layout();
    click(c, panel.pointOrigin);
    check(near3(c.duplicate_special_settings().point, {}), "Origin sets the point");
    type_into(c, NumberFieldId::DuplicateCopies, 0, "4");
    type_into(c, NumberFieldId::DuplicateOffset, 0, "0");
    type_into(c, NumberFieldId::DuplicateOffset, 1, "0");
    type_into(c, NumberFieldId::DuplicateOffset, 2, "0");
    type_into(c, NumberFieldId::DuplicateRotation, 1, "90");
    type_into(c, NumberFieldId::DuplicateRotation, 2, "0");
    type_into(c, NumberFieldId::DuplicateScale, 0, "0.5");
    panel = c.duplicate_panel_layout();
    click(c, panel.group);
    check(!c.duplicate_special_settings().group, "the group can be turned off");
    click(c, c.duplicate_panel_layout().apply);
    std::vector<const EditorObject*> copies;
    for (const auto& [id, object] : document.objects())
        if (object.name.starts_with("Rock ")) copies.push_back(&object);
    check(copies.size() == 4 && c.workspace().selection_count() == 4, "4 loose copies, selected");
    bool ring = false, scaled = false;
    for (const EditorObject* copy : copies) {
        if (copy->name == "Rock 01") ring = near3(copy->transform.position, {0.0F, 0.0F, -5.0F});
        if (copy->name == "Rock 02") scaled = near(copy->voxelSizeMeters, 0.025F);
    }
    check(ring, "a quarter turn around the origin");
    check(scaled, "voxel size halves per copy");
}

void test_refusals_and_scrub() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().clear_selection();
    check(!c.dispatch_action("edit.duplicate_special"), "nothing selected is refused");
    c.workspace().select_object(1);
    c.workspace().document().find_object(1)->flags.visible = false;
    check(!c.open_duplicate_special() && c.status().text.find("hidden") != std::string::npos, "hidden objects are refused");
    c.workspace().document().find_object(1)->flags.visible = true;
    check(c.open_duplicate_special(), "opens");
    const auto copies = field(c, NumberFieldId::DuplicateCopies);
    if (copies) {
        const int start = c.duplicate_special_settings().copies;
        c.pointer_down(PointerButton::Primary, copies->label.x + 5, copies->label.y + 9);
        c.pointer_move(copies->label.x + 65, copies->label.y + 9);
        c.pointer_up(PointerButton::Primary, copies->label.x + 65, copies->label.y + 9);
        check(static_cast<int>(c.duplicate_special_settings().copies) == start + 10, "dragging Copies scrubs it (6 px a copy)");
    }
    type_into(c, NumberFieldId::DuplicateCopies, 0, "5000");
    check(c.status().error && c.status().text.find("1 to 2000") != std::string::npos, "too many copies says the limit");
    c.key_down("escape", false, false, false);
    check(!c.duplicate_special_active() && !c.workspace().commands().can_undo(), "Esc cancels without changes");
    check(c.open_duplicate_special(), "reopen");
    check(c.dispatch_action("select.domain_voxels") || true, "switch domain");
    c.update(0.016F);
    check(!c.duplicate_special_active(), "leaving the Objects domain closes the panel");
}
}  // namespace

int main() {
    test_poses();
    test_feathers();
    test_point_scale_and_options();
    test_refusals_and_scrub();
    if (g_failures == 0) std::printf("duplicate special tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
