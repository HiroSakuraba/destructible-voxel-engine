#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__));                   \
    } while (false)
using namespace dve;
using namespace dve::editor;
EditorDocument scene() {
    EditorDocument doc("Slice tests");
    EditorObject o(1, "Wall");
    for (int x = -2; x < 2; ++x)
        (void)o.voxels->set_voxel({x, 0, 0}, static_cast<MaterialId>(x + 3));
    o.anchors = {{-2, 0, 0}, {1, 0, 0}};
    doc.add_object(std::move(o));
    doc.mark_clean();
    return doc;
}
void slice_tests() {
    EditorWorkspace w(scene());
    EditorVoxelSliceSession slice;
    std::string error;
    CHECK(slice.begin(w.document(), 1, {0, 0, 0}, {1, 0, 0}, &error));
    const auto hash = w.document().find_object(1)->voxels->state_hash();
    const auto original = capture_voxel_object_state(*w.document().find_object(1));
    const auto restored = [&] {
        const auto current = capture_voxel_object_state(*w.document().find_object(1));
        return current.anchors == original.anchors && current.voxels.size() == original.voxels.size() &&
               std::equal(current.voxels.begin(), current.voxels.end(), original.voxels.begin(),
                          [](const auto& a, const auto& b) {
                              return a.voxel == b.voxel && a.material == b.material;
                          });
    };
    CHECK(!w.document().dirty());
    CHECK(slice.cells().size() == 4);
    slice.cancel();
    CHECK(w.document().find_object(1)->voxels->state_hash() == hash);
    CHECK(w.commands().size() == 0);
    for (auto output : {VoxelSliceOutput::Front, VoxelSliceOutput::Back, VoxelSliceOutput::Separate}) {
        CHECK(slice.begin(w.document(), 1, {0, 0, 0}, {1, 0, 0}, &error));
        CHECK(slice.configure(w.document(), {0, 0, 0}, {1, 0, 0}, output, &error));
        CHECK(slice.commit(w).success);
        CHECK(w.document().find_object(1)->voxels->occupied_voxel_count() == 2);
        if (output == VoxelSliceOutput::Separate) {
            CHECK(w.document().objects().size() == 2);
            const auto& back = w.document().objects().rbegin()->second;
            CHECK(back.voxels->occupied_voxel_count() == 2);
            CHECK(back.anchors.contains({-2, 0, 0}));
            CHECK(back.voxels->material_at({-2, 0, 0}) == 1);
        }
        CHECK(w.commands().undo(w.document()).success);
        CHECK(w.document().objects().size() == 1);
        CHECK(restored());
        CHECK(w.document().find_object(1)->anchors.size() == 2);
        CHECK(w.commands().redo(w.document()).success);
        CHECK(w.commands().undo(w.document()).success);
    }
    CHECK(!slice.begin(w.document(), 1, {0, 0, 0}, {0, 0, 0}, &error));
    CHECK(!slice.begin(w.document(), 1, {std::numeric_limits<float>::infinity(), 0, 0}, {1, 0, 0}, &error));
    CHECK(slice.begin(w.document(), 1, {10, 0, 0}, {1, 0, 0}, &error));
    CHECK(!slice.commit(w).success);
    CHECK(slice.begin(w.document(), 1, {0, 0, 0}, {1, 0, 0}, &error));
    (void)w.document().find_object(1)->voxels->set_voxel({0, 0, 0}, 5);
    CHECK(!slice.commit(w).success);
    CHECK(w.document().find_object(1)->voxels->material_at({0, 0, 0}) == 5);
    auto& o = *w.document().find_object(1);
    o.transform = make_rigid_transform({3, 4, 5}, quaternion_from_axis_angle({0, 0, 1}, 1.57079632679F));
    CHECK(slice.begin(w.document(), 1, {3, 4, 5}, {0, 1, 0}, &error));
    CHECK(std::count_if(slice.cells().begin(), slice.cells().end(), [](const auto& c) { return c.front; }) ==
          2);
}
class Canvas final : public IEditorCanvas {
  public:
    mutable bool position{}, rotation{}, valueBelow{};
    mutable int posY{};
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int y, std::string_view s, EditorColor) const override {
        if (s == "Position") {
            position = true;
            posY = y;
        }
        if (s == "Rotation")
            rotation = true;
        if (position && y == posY + 18)
            valueBelow = true;
    }
    int text_width(std::string_view s) const override { return static_cast<int>(s.size()) * 7; }
};
void native_tests() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(scene()));
    auto& c = *controller;
    c.resize(1280, 720);
    c.workspace().select_object(1);
    c.update(0);
    CHECK(c.layout().inspectorStackedFields);
    CHECK(c.layout().inspectorFields[0].height == 40);
    Canvas canvas;
    render_native_editor(canvas, c, 1280, 720);
    CHECK(canvas.position && canvas.rotation && canvas.valueBelow);
    const auto field = c.layout().inspectorFields[0];
    c.pointer_down(PointerButton::Primary, field.x + 5, field.y + 29);
    CHECK(c.text_edit().kind == TextEditKind::Position);
    c.key_down("escape", false, false, false);
    CHECK(c.text_edit().kind == TextEditKind::Inactive);
    CHECK(c.dispatch_action("voxel.slice"));
    CHECK(c.voxel_slice().active());
    c.key_down("3", false, false, false);
    CHECK(c.voxel_slice().output() == VoxelSliceOutput::Separate);
    c.key_down("escape", false, false, false);
    CHECK(!c.voxel_slice().active());
    CHECK(c.workspace().commands().size() == 0);
    CHECK(c.dispatch_action("voxel.slice"));
    CHECK(c.configure_voxel_slice({0, 0, 0}, {1, 0, 0}, VoxelSliceOutput::Back));
    CHECK(c.dispatch_action("voxel.slice_commit"));
    CHECK(c.workspace().commands().size() == 1);
    CHECK(c.dispatch_action("edit.undo"));
    CHECK(c.workspace().document().find_object(1)->voxels->occupied_voxel_count() == 4);
    CHECK(c.dispatch_action("voxel.slice"));
    c.workspace().clear_selection();
    c.update(0);
    CHECK(!c.voxel_slice().active());
}
int main() {
    try {
        slice_tests();
        native_tests();
        std::cout << "Authoring gaps checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
