// Per-frame render work is cached: the voxel draw list (projection + sort), the
// camera preview list, and selection diagnostics rebuild only on change, and
// hierarchy_order is O(n) (was O(n^2) via children_of per node).
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct NullCanvas final : IEditorCanvas {
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int, std::string_view, EditorColor) const override {}
    [[nodiscard]] int text_width(std::string_view v) const override { return static_cast<int>(v.size()) * 7; }
};

std::vector<EditorObjectId> reference_order(const EditorDocument& document) {
    std::vector<EditorObjectId> result;
    std::function<void(EditorObjectId)> visit = [&](EditorObjectId id) {
        result.push_back(id);
        for (EditorObjectId child : document.children_of(id)) visit(child);
    };
    for (EditorObjectId root : document.root_objects()) visit(root);
    return result;
}

void test_draw_list_cache() {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(1280, 800);
    NullCanvas canvas;
    render_native_editor(canvas, controller, 1280, 800);
    const auto base = controller.render_cache_stats();
    for (int i = 0; i < 10; ++i) render_native_editor(canvas, controller, 1280, 800);
    (void)controller.draw_item_count();
    auto stats = controller.render_cache_stats();
    require(stats.drawListRebuilds == base.drawListRebuilds, "idle frames rebuilt the voxel draw list");
    const std::size_t count = controller.draw_item_count();
    require(count > 0U, "demo scene produced no draw items");

    // Voxel edit -> rebuild, and the new voxel is in the list.
    EditorObject* wall = controller.workspace().document().find_object(1002);
    require(wall != nullptr, "demo wall missing");
    (void)wall->voxels->set_voxel({0, 2, 0}, 6);  // fills a doorway voxel
    render_native_editor(canvas, controller, 1280, 800);
    stats = controller.render_cache_stats();
    require(stats.drawListRebuilds == base.drawListRebuilds + 1U, "voxel edit did not rebuild the draw list");
    require(controller.draw_item_count() == count + 1U, "draw list does not reflect the voxel edit");

    // Camera move -> rebuild.
    controller.camera().position.x += 0.5F;
    render_native_editor(canvas, controller, 1280, 800);
    require(controller.render_cache_stats().drawListRebuilds == base.drawListRebuilds + 2U, "camera move did not rebuild");

    // Selection change -> rebuild (selected flag lives in the items).
    controller.workspace().select_object(1003);
    render_native_editor(canvas, controller, 1280, 800);
    require(controller.render_cache_stats().drawListRebuilds == base.drawListRebuilds + 3U, "selection did not rebuild");
    bool sawSelected = false;
    for (const auto& item : controller.draw_items()) sawSelected = sawSelected || (item.selected && item.objectId == 1003);
    require(sawSelected, "selected flag missing after selection change");

    // Visibility toggle -> rebuild, beam items disappear.
    controller.workspace().document().find_object(1003)->flags.visible = false;
    render_native_editor(canvas, controller, 1280, 800);
    for (const auto& item : controller.draw_items()) require(item.objectId != 1003, "hidden object still drawn");

    // Transform change -> rebuild.
    const auto beforeMove = controller.render_cache_stats().drawListRebuilds;
    controller.workspace().document().find_object(1004)->transform.position.y += 1.0F;
    render_native_editor(canvas, controller, 1280, 800);
    require(controller.render_cache_stats().drawListRebuilds == beforeMove + 1U, "transform change did not rebuild");

    // Resize -> rebuild.
    controller.resize(1024, 700);
    render_native_editor(canvas, controller, 1024, 700);
    require(controller.render_cache_stats().drawListRebuilds == beforeMove + 2U, "viewport resize did not rebuild");
    std::printf("draw list cache: OK (%llu rebuilds over %d frames)\n",
                static_cast<unsigned long long>(controller.render_cache_stats().drawListRebuilds), 17);
}

void test_selection_diagnostics_cache() {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(1280, 800);
    controller.workspace().select_object(1004);
    NullCanvas canvas;
    render_native_editor(canvas, controller, 1280, 800);
    const auto base = controller.render_cache_stats().selectionDiagnosticsRebuilds;
    const double mass = controller.selection_diagnostics().massKilograms;
    for (int i = 0; i < 10; ++i) render_native_editor(canvas, controller, 1280, 800);
    require(controller.render_cache_stats().selectionDiagnosticsRebuilds <= base + 1U,
            "selection diagnostics recomputed every frame");
    const auto afterIdle = controller.render_cache_stats().selectionDiagnosticsRebuilds;
    // Material density change -> recompute with the new mass.
    EditorMaterialEntry* wood = controller.materials().find(3);
    require(wood != nullptr, "material 3 missing");
    wood->definition.densityKilogramsPerCubicMeter *= 2.0F;
    const double doubled = controller.selection_diagnostics().massKilograms;
    require(std::abs(doubled - 2.0 * mass) < 1e-6 * std::max(1.0, mass), "diagnostics ignored density change");
    // Voxel removal -> recompute.
    (void)controller.workspace().document().find_object(1004)->voxels->set_voxel({0, 0, 0}, kAirMaterial);
    require(controller.selection_diagnostics().massKilograms < doubled, "diagnostics ignored voxel edit");
    require(controller.render_cache_stats().selectionDiagnosticsRebuilds == afterIdle + 2U, "unexpected rebuild count");
    std::printf("selection diagnostics cache: OK\n");
}

void test_camera_preview_cache() {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.resize(1280, 800);
    (void)controller.create_camera_rig_from_view("Probe Cam");
    require(controller.selected_camera_rig().has_value(), "camera rig not selected");
    NullCanvas canvas;
    render_native_editor(canvas, controller, 1280, 800);
    const auto base = controller.render_cache_stats().previewDrawListRebuilds;
    require(base >= 1U, "camera preview was not drawn through the cache");
    for (int i = 0; i < 10; ++i) render_native_editor(canvas, controller, 1280, 800);
    require(controller.render_cache_stats().previewDrawListRebuilds == base, "camera preview rebuilt every frame");
    (void)controller.workspace().document().find_object(1002)->voxels->set_voxel({0, 2, 0}, 6);
    render_native_editor(canvas, controller, 1280, 800);
    require(controller.render_cache_stats().previewDrawListRebuilds == base + 1U, "camera preview ignored an edit");
    std::printf("camera preview cache: OK\n");
}

void test_hierarchy_order_linear() {
    for (int n : {1, 7, 64, 5000}) {
        EditorDocument document("hierarchy");
        for (int i = 0; i < n; ++i) {
            EditorObject object(static_cast<EditorObjectId>(10 + i), "Node " + std::to_string(i));
            // Mix of parents with lower AND higher ids, several roots, deep chains.
            if (i % 5 != 0) object.parent = static_cast<EditorObjectId>(10 + ((i * 7 + 3) % n));
            if (object.parent && *object.parent == object.id) object.parent.reset();
            document.add_object(std::move(object));
        }
        // Break cycles so the document is a forest (validate() requires it).
        for (int pass = 0; pass < n; ++pass) {
            bool changed = false;
            for (const auto& [id, object] : document.objects()) {
                std::set<EditorObjectId> seen{id};
                std::optional<EditorObjectId> cursor = object.parent;
                while (cursor) {
                    if (!seen.insert(*cursor).second) {
                        document.find_object(id)->parent.reset();
                        changed = true;
                        break;
                    }
                    cursor = document.find_object(*cursor)->parent;
                }
            }
            if (!changed) break;
        }
        const auto expected = n <= 64 ? reference_order(document) : std::vector<EditorObjectId>{};
        const auto t0 = std::chrono::steady_clock::now();
        const auto actual = document.hierarchy_preorder();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        require(actual.size() == static_cast<std::size_t>(n), "hierarchy_preorder lost objects");
        if (n <= 64) require(actual == expected, "hierarchy_preorder differs from the recursive order");
        if (n == 5000) {
            std::printf("hierarchy_preorder(5000): %.3f ms\n", ms);
            require(ms < 100.0, "hierarchy_preorder is too slow for 5000 objects");
        }
    }
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    require(controller.hierarchy_order() == reference_order(controller.workspace().document()),
            "controller hierarchy_order changed order");
    std::printf("hierarchy order: OK\n");
}

} // namespace

int main() {
    try {
        test_draw_list_cache();
        test_selection_diagnostics_cache();
        test_camera_preview_cache();
        test_hierarchy_order_linear();
        std::printf("editor render cache tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
