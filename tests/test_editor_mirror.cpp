// Mirror objects across a world plane, as copies or in place, with real geometry flips:
// voxel cells flip on their grid, polygon meshes and subdivision cages flip and keep their
// winding outward, persistent IDs and control faces. Children follow; one undo step.
#include "dve/editor_mirror.hpp"
#include "dve/editor_modeling_topology.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_mesh_authoring.hpp"
#include "dve/voxel_metric.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near3(Float3 a, Float3 b, float tolerance = 1.0e-3F) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
}
Float3 reflect(Float3 p, int axis, float plane) {
    if (axis == 0) p.x = 2.0F * plane - p.x;
    else if (axis == 1) p.y = 2.0F * plane - p.y;
    else p.z = 2.0F * plane - p.z;
    return p;
}
Float3 flip(Float3 p, int axis) { return reflect(p, axis, 0.0F); }

void test_pose_math() {
    const RigidTransform plain = make_rigid_transform({3.0F, 1.0F, 0.0F}, {});
    MirroredPose pose = mirror_pose(plain, 0, {1.0F, 0.0F, 0.0F});
    check(near3(pose.transform.position, {-1.0F, 1.0F, 0.0F}) && pose.localAxis == 0, "an axis-aligned object flips its own X");
    // Any orientation: reflecting the world geometry equals T' applied to the locally flipped geometry.
    for (const Float3 euler : {Float3{0.0F, 1.5708F, 0.0F}, Float3{0.3F, 0.7F, -1.1F}, Float3{2.0F, -0.4F, 0.9F}}) {
        const RigidTransform t = make_rigid_transform({0.5F, -2.0F, 4.0F}, quaternion_from_euler_xyz(euler));
        for (int axis = 0; axis < 3; ++axis) {
            const MirroredPose m = mirror_pose(t, axis, {0.25F, 0.5F, -1.0F});
            bool exact = true;
            for (const Float3 p : {Float3{1, 0, 0}, Float3{0, 2, 0}, Float3{0.3F, -0.7F, 1.9F}}) {
                const float plane = axis == 0 ? 0.25F : axis == 1 ? 0.5F : -1.0F;
                exact = exact && near3(reflect(transform_point(t, p), axis, plane), transform_point(m.transform, flip(p, m.localAxis)));
            }
            check(exact, "reflection is exact for a rotated object, axis " + std::to_string(axis));
        }
    }
    VoxelObjectState state;
    state.voxels = {{{0, 0, 0}, 1}, {{2, 1, 0}, 2}};
    state.anchors = {{2, 1, 0}};
    const VoxelObjectState flipped = mirror_voxel_state(state, 0);
    check(flipped.voxels.size() == 2 && flipped.voxels[0].voxel == Int3{-3, 1, 0} && flipped.voxels[0].material == 2 &&
              flipped.voxels[1].voxel == Int3{-1, 0, 0} && flipped.anchors.front() == Int3{-3, 1, 0},
          "voxel cells flip i -> -i-1, anchors too");
}

// An L-shaped voxel object: 3 cells along X plus one up at the end.
EditorObject make_l(EditorObjectId id, Float3 position, Quaternion rotation = {}) {
    EditorObject object(id, "Bracket");
    object.voxelSizeMeters = 0.5F;
    object.transform = make_rigid_transform(position, rotation);
    for (int x = 0; x < 3; ++x) (void)object.voxels->set_voxel({x, 0, 0}, 1);
    (void)object.voxels->set_voxel({2, 1, 0}, 2);
    object.anchors.insert({0, 0, 0});
    return object;
}

std::set<std::tuple<int, int, int, int>> world_cells(const EditorDocument& document, EditorObjectId id) {
    const EditorObject* object = document.find_object(id);
    std::set<std::tuple<int, int, int, int>> cells;
    const Float3 spacing = multiply(object->voxelScale, object->voxelSizeMeters);
    for (const Int3 cell : all_occupied_voxels(*object->voxels)) {
        const Float3 centre = voxel_to_world(object->transform, spacing, {cell.x + 0.5F, cell.y + 0.5F, cell.z + 0.5F});
        cells.insert({static_cast<int>(std::lround(centre.x * 100)), static_cast<int>(std::lround(centre.y * 100)),
                      static_cast<int>(std::lround(centre.z * 100)), object->voxels->material_at(cell)});
    }
    return cells;
}
std::set<std::tuple<int, int, int, int>> reflected(const std::set<std::tuple<int, int, int, int>>& cells, int axis, float plane) {
    std::set<std::tuple<int, int, int, int>> result;
    for (auto [x, y, z, m] : cells) {
        const Float3 p = reflect({x / 100.0F, y / 100.0F, z / 100.0F}, axis, plane);
        result.insert({static_cast<int>(std::lround(p.x * 100)), static_cast<int>(std::lround(p.y * 100)), static_cast<int>(std::lround(p.z * 100)), m});
    }
    return result;
}

std::unique_ptr<NativeEditorController> make_controller(EditorDocument document) {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    controller->resize(1280, 720);
    return controller;
}

void test_voxel_copy_and_in_place() {
    EditorDocument document("Mirror");
    document.add_object(make_l(1, {1.0F, 0.0F, 0.0F}, quaternion_from_euler_xyz({0.0F, 0.5236F, 0.0F})));
    EditorObject child(2, "Hook");
    child.parent = 1;
    (void)child.voxels->set_voxel({0, 0, 0}, 3);
    child.voxelSizeMeters = 0.25F;
    child.transform.position = {2.5F, 1.0F, 0.3F};
    document.add_object(std::move(child));
    auto owner = make_controller(std::move(document));
    NativeEditorController& c = *owner;
    EditorDocument& doc = c.workspace().document();
    c.workspace().select_object(1);
    check(c.dispatch_action("transform.pivot_origin"), "pivot at the world origin");
    const auto before = world_cells(doc, 1);
    const auto childBefore = world_cells(doc, 2);
    const std::size_t count = doc.objects().size();
    check(c.dispatch_action("edit.mirror_copy_x"), "Mirror Copy X: " + c.status().text);
    check(doc.objects().size() == count + 2, "the object and its child are copied");
    const auto copy = c.workspace().selected_object();
    const EditorObject* copyObject = copy ? doc.find_object(*copy) : nullptr;
    check(copyObject && copyObject->name == "Bracket (mirror)", "the copy is selected");
    if (copyObject) {
        check(world_cells(doc, copyObject->id) == reflected(before, 0, 0.0F), "every voxel lands at its mirror image (rotated object)");
        const auto children = doc.children_of(copyObject->id);
        check(children.size() == 1 && world_cells(doc, children.front()) == reflected(childBefore, 0, 0.0F),
              "the child is copied, mirrored and parented to the copy");
        check(copyObject->anchors.size() == 1, "anchors come along");
    }
    check(world_cells(doc, 1) == before, "the source is untouched");
    check(c.workspace().commands().undo(doc).success && doc.objects().size() == count, "one undo removes the copy");

    // In place across Y through the origin.
    c.workspace().select_object(1);
    (void)c.dispatch_action("transform.pivot_origin");
    check(c.mirror_selection(1, false), "Mirror Y in place: " + c.status().text);
    check(world_cells(doc, 1) == reflected(before, 1, 0.0F) && world_cells(doc, 2) == reflected(childBefore, 1, 0.0F),
          "in place: object and child mirror");
    check(c.workspace().commands().undo(doc).success && world_cells(doc, 1) == before && world_cells(doc, 2) == childBefore,
          "one undo restores both exactly");

    doc.find_object(1)->flags.locked = true;
    check(!c.mirror_selection(0, false) && c.status().text.find("Unlock") != std::string::npos, "in place refuses locked objects");
    doc.find_object(1)->flags.locked = false;
}

double signed_volume(const CookedPolygonAsset& mesh) {
    double volume = 0.0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Float3 a = mesh.vertices[mesh.indices[i]].position, b = mesh.vertices[mesh.indices[i + 1]].position,
                     c = mesh.vertices[mesh.indices[i + 2]].position;
        volume += (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
    }
    return volume;
}

void test_meshes() {
    // A box stretched along X and off-centre, so the flip is visible; with persistent IDs.
    EditorDocument document("Mesh mirror");
    EditorObject box(1, "Box");
    box.polygon = make_editor_polygon_cube(1, 1.0F);
    for (PolygonVertex& v : box.polygon->vertices) v.position.x = v.position.x * 2.0F + 1.5F;
    refresh_editor_polygon(*box.polygon, true);
    std::string error;
    check(ensure_modeling_topology(box, &error), "IDs: " + error);
    const double volume = signed_volume(*box.polygon);
    document.add_object(std::move(box));
    EditorObject cage(2, "Cage");
    cage.polygon = make_editor_polygon_cube(2, 1.0F);
    for (PolygonVertex& v : cage.polygon->vertices) v.position.z += 2.0F;
    refresh_editor_polygon(*cage.polygon, true);
    SubdivisionSettings settings;
    freeze_subdivision_topology(*cage.polygon, settings);
    cage.subdivision = settings;
    check(ensure_modeling_topology(cage, &error), "cage IDs: " + error);
    document.add_object(std::move(cage));
    auto owner = make_controller(std::move(document));
    NativeEditorController& c = *owner;
    EditorDocument& doc = c.workspace().document();
    c.workspace().select_object(1);
    (void)c.dispatch_action("transform.pivot_origin");
    const auto topologyBefore = *doc.find_object(1)->modelingTopology;
    check(c.mirror_selection(0, false), "mirror the mesh in place: " + c.status().text);
    const EditorObject* mirrored = doc.find_object(1);
    // The cube spans 0..1; stretched it spans x 1.5..3.5.
    check(near3(object_world_bounds(*mirrored).minimum, {-3.5F, 0.0F, 0.0F}) && near3(object_world_bounds(*mirrored).maximum, {-1.5F, 1.0F, 1.0F}),
          "the mesh moves to the other side of the plane");
    check(signed_volume(*mirrored->polygon) > 0.0 && std::abs(signed_volume(*mirrored->polygon) - volume) < 1e-4,
          "faces still point outward (winding reversed)");
    check(mirrored->modelingTopology && mirrored->modelingTopology->vertices == topologyBefore.vertices &&
              mirrored->modelingTopology->faces == topologyBefore.faces,
          "persistent vertex and face IDs are kept");
    check(validate_polygon_asset(*mirrored->polygon).code == PolygonAssetErrorCode::NoError, "the mesh is valid");
    check(c.workspace().commands().undo(doc).success && doc.find_object(1)->modelingTopology == topologyBefore,
          "undo restores the IDs");

    // Saved UV seams and pins survive the mirror (pins follow the swapped corners).
    {
        PolygonUvAuthoring state;
        state.layoutHash = polygon_uv_layout_hash(*doc.find_object(1)->polygon);
        state.seams[0].insert(*polygon_all_components(*doc.find_object(1)->polygon, PolygonSelectionMode::Edge).begin());
        state.pins[0] = {1U, 3U};
        auto command = make_polygon_uv_authoring_command(doc, *doc.find_object(1), state, &error);
        check(command && c.workspace().commands().execute(doc, std::move(command)).success, "save UV flags: " + error);
        check(c.mirror_selection(0, false), "mirror with saved UV flags: " + c.status().text);
        const auto after = read_polygon_uv_authoring(*doc.find_object(1), &error);
        check(after && after->seams[0].size() == 1 && after->pins[0] == std::set<std::uint32_t>{2U, 3U},
              "seams kept, pins on the swapped corners: " + error);
        check(c.mirror_selection(0, true), "mirror copy with saved UV flags: " + c.status().text);
        const auto copy = c.workspace().selected_object();
        const auto copied = copy ? read_polygon_uv_authoring(*doc.find_object(*copy), &error) : std::nullopt;
        check(copied && copied->pins[0] == std::set<std::uint32_t>{1U, 3U}, "the copy's record is valid too (mirrored twice): " + error);
    }

    c.workspace().select_object(2);
    check(c.mirror_selection(2, true), "mirror copy of a subdivision cage: " + c.status().text);
    const auto copy = c.workspace().selected_object();
    const EditorObject* cageCopy = copy ? doc.find_object(*copy) : nullptr;
    check(cageCopy && cageCopy->subdivision && evaluate_subdivision(*cageCopy->polygon, *cageCopy->subdivision).mesh.has_value(),
          "the mirrored cage still subdivides");
    check(cageCopy && near3(object_world_bounds(*cageCopy).minimum, {0.0F, 0.0F, -3.0F}), "and sits mirrored across Z");
    check(cageCopy && signed_volume(*cageCopy->polygon) > 0.0, "with outward faces");
}

void test_refusals() {
    EditorDocument document("Refusals");
    document.add_object(make_l(1, {}));
    EditorObject text(2, "Sign");
    text.text3d.emplace();
    document.add_object(std::move(text));
    auto owner = make_controller(std::move(document));
    NativeEditorController& c = *owner;
    c.workspace().clear_selection();
    check(!c.dispatch_action("edit.mirror_x"), "nothing selected is refused");
    c.workspace().select_object(2);
    check(!c.mirror_selection(0, true) && c.status().text.find("3D text") != std::string::npos, "3D text is refused with the reason");
    c.workspace().select_object(1);
    c.workspace().document().find_object(1)->prefabLink.emplace();
    check(!c.mirror_selection(0, true) && c.status().text.find("prefab") != std::string::npos, "prefab instances are refused");
    c.workspace().document().find_object(1)->prefabLink.reset();
    (void)c.dispatch_action("select.domain_voxels");
    check(!c.mirror_selection(0, true), "the Voxels domain is refused");
}
}  // namespace

int main() {
    test_pose_math();
    test_voxel_copy_and_in_place();
    test_meshes();
    test_refusals();
    if (g_failures == 0) std::printf("mirror tests passed\n");
    return g_failures == 0 ? 0 : 1;
}

