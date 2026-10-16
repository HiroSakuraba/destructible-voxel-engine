// Half-model mirroring of polygon meshes and subdivision cages (Polygon > Mirror Mesh, Mirror
// Selected Faces, Live Mirror, Apply Live Mirror): the mirrored half is welded at the seam, faces
// on the plane are removed, identities, creases and control faces carry over; one undo.
#include "dve/editor_mirror.hpp"
#include "dve/editor_modeling_topology.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_polygon.hpp"

#include <cmath>
#include <cstdio>
#include <map>
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

double signed_volume(const CookedPolygonAsset& mesh) {
    double volume = 0.0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Float3 a = mesh.vertices[mesh.indices[i]].position, b = mesh.vertices[mesh.indices[i + 1]].position,
                     c = mesh.vertices[mesh.indices[i + 2]].position;
        volume += (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
    }
    return volume;
}
// Closed and consistently wound: every directed geometric edge appears exactly once and its
// reverse exactly once.
bool watertight(const CookedPolygonAsset& mesh) {
    std::map<std::tuple<float, float, float>, int> ids;
    const auto id = [&](std::uint32_t v) {
        const Float3 p = mesh.vertices[v].position;
        return ids.emplace(std::tuple{p.x, p.y, p.z}, static_cast<int>(ids.size())).first->second;
    };
    std::map<std::pair<int, int>, int> directed;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        for (int j = 0; j < 3; ++j) ++directed[{id(mesh.indices[i + j]), id(mesh.indices[i + (j + 1) % 3])}];
    for (const auto& [edge, count] : directed) {
        const auto reverse = directed.find({edge.second, edge.first});
        if (count != 1 || reverse == directed.end() || reverse->second != 1) return false;
    }
    return true;
}
std::size_t geometric_vertices(const CookedPolygonAsset& mesh) {
    std::set<std::tuple<float, float, float>> points;
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        const Float3 p = mesh.vertices[mesh.indices[i]].position;
        points.insert({p.x, p.y, p.z});
    }
    return points.size();
}

void test_core() {
    // The editor cube spans 0..1: its x = 0 side lies on the YZ plane, like a half model.
    const CookedPolygonAsset half = make_editor_polygon_cube(1);
    MeshMirrorResult result = mirror_polygon_half(half, nullptr, 0, nullptr, 1e-5F);
    check(bool(result), "mirror the half: " + result.error);
    check(result.mesh.indices.size() / 3 == 20 && result.removed == 2, "the two seam-plane triangles go, ten are mirrored");
    check(watertight(result.mesh), "the result is closed and consistently wound");
    check(std::abs(signed_volume(result.mesh) - 2.0) < 1e-4, "a 2 x 1 x 1 box, faces outward");
    check(geometric_vertices(result.mesh) == 12, "4 seam corners shared, 4 new ones: 12 corners");
    check(validate_polygon_asset(result.mesh).code == PolygonAssetErrorCode::NoError, "valid mesh");

    CookedPolygonAsset centred = half;
    for (PolygonVertex& v : centred.vertices) v.position.x -= 0.5F;
    refresh_editor_polygon(centred, true);
    result = mirror_polygon_half(centred, nullptr, 0, nullptr, 1e-5F);
    check(!result && result.error.find("crosses the mirror plane") != std::string::npos, "a mesh across the plane is refused");

    // Selected faces only: the +Y face (two triangles) copied across X.
    std::set<std::uint32_t> top;
    for (std::uint32_t f = 0; f < half.indices.size() / 3; ++f) {
        bool up = true;
        for (int j = 0; j < 3; ++j) up = up && half.vertices[half.indices[f * 3 + j]].position.y > 0.99F;
        if (up) top.insert(f);
    }
    result = mirror_polygon_half(half, nullptr, 0, &top, 1e-5F);
    check(bool(result) && result.newFaces.size() == 2 && result.mesh.indices.size() / 3 == 14 && result.removed == 0,
          "two faces copied, nothing removed");
    check(geometric_vertices(result.mesh) == 10, "the copy shares its two seam corners");
}

std::unique_ptr<NativeEditorController> make_controller(bool cage) {
    EditorDocument document("Half");
    EditorObject object(1, cage ? "Cage" : "Half");
    object.polygon = make_editor_polygon_cube(1);
    if (cage) {
        SubdivisionSettings settings;
        settings.level = 1;
        freeze_subdivision_topology(*object.polygon, settings);
        object.subdivision = settings;
    }
    std::string error;
    check(ensure_modeling_topology(object, &error), "IDs: " + error);
    document.add_object(std::move(object));
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    controller->resize(1280, 720);
    controller->workspace().select_object(1);
    controller->camera().position = {0.5F, 2.0F, 5.0F};
    controller->camera().target = {0.0F, 0.5F, 0.5F};
    return controller;
}

void test_mirror_mesh_command() {
    auto owner = make_controller(false);
    NativeEditorController& c = *owner;
    EditorDocument& doc = c.workspace().document();
    const auto idsBefore = *doc.find_object(1)->modelingTopology;
    const std::set<ModelingId> oldVertexIds(idsBefore.vertices.begin(), idsBefore.vertices.end());
    check(c.dispatch_action("mirror.mesh_x"), "Polygon > Mirror Mesh X: " + c.status().text);
    const EditorObject* mesh = doc.find_object(1);
    check(watertight(*mesh->polygon) && std::abs(signed_volume(*mesh->polygon) - 2.0) < 1e-4, "closed 2 x 1 x 1 box");
    check(c.status().text.find("seam vertices welded") != std::string::npos && c.status().text.find("2 faces on the plane removed") != std::string::npos,
          "the status says what happened: " + c.status().text);
    bool kept = mesh->modelingTopology.has_value();
    if (kept)
        for (std::size_t i = 0; i < idsBefore.vertices.size(); ++i) kept = kept && mesh->modelingTopology->vertices[i] == idsBefore.vertices[i];
    check(kept, "the original vertices keep their persistent IDs");
    bool fresh = mesh->modelingTopology.has_value();
    if (fresh)
        for (std::size_t i = idsBefore.vertices.size(); i < mesh->modelingTopology->vertices.size(); ++i)
            fresh = fresh && !oldVertexIds.contains(mesh->modelingTopology->vertices[i]);
    check(fresh, "the mirrored half gets new IDs");
    check(c.workspace().commands().undo(doc).success && doc.find_object(1)->polygon->indices.size() == 36 &&
              doc.find_object(1)->modelingTopology == idsBefore,
          "one undo restores the half and its IDs");

    // Selected faces in the polygon domain.
    check(c.dispatch_action("polygon.face_mode"), "face mode");
    std::set<std::uint64_t> top;
    const CookedPolygonAsset& source = *doc.find_object(1)->polygon;
    for (std::uint32_t f = 0; f < source.indices.size() / 3; ++f) {
        bool up = true;
        for (int j = 0; j < 3; ++j) up = up && source.vertices[source.indices[f * 3 + j]].position.y > 0.99F;
        if (up) top.insert(f);
    }
    c.select_polygon_components(top, SelectionOperation::Replace);
    check(c.dispatch_action("mirror.faces_x"), "Mirror Selected Faces X: " + c.status().text);
    check(doc.find_object(1)->polygon->indices.size() / 3 == 14 && c.polygon_selection().components.size() == 2 &&
              *c.polygon_selection().components.begin() >= 12,
          "two faces copied and the copies selected");
    check(c.workspace().commands().undo(doc).success && doc.find_object(1)->polygon->indices.size() == 36, "undo");

    doc.find_object(1)->flags.locked = true;
    check(!c.mirror_mesh(0, false), "a locked mesh is refused");
    doc.find_object(1)->flags.locked = false;
}

void test_cage_and_creases() {
    auto owner = make_controller(true);
    NativeEditorController& c = *owner;
    EditorDocument& doc = c.workspace().document();
    EditorObject* cage = doc.find_object(1);
    // A crease on an edge of the top face, away from the y = 0 plane.
    std::uint64_t topEdge = 0;
    for (const std::uint64_t key : subdivision_control_edges(*cage->polygon, &*cage->subdivision)) {
        const Float3 a = cage->polygon->vertices[static_cast<std::uint32_t>(key >> 32U)].position;
        const Float3 b = cage->polygon->vertices[static_cast<std::uint32_t>(key)].position;
        if (a.y > 0.99F && b.y > 0.99F) { topEdge = key; break; }
    }
    cage->subdivision->edgeCreases[topEdge] = 2.0F;
    check(c.mirror_mesh(1, false), "mirror the cage across Y: " + c.status().text);
    const EditorObject* mirrored = doc.find_object(1);
    check(mirrored->subdivision && subdivision_control_face_triangles(*mirrored->polygon, &*mirrored->subdivision).size() == 10,
          "5 quads kept + 5 mirrored = 10 control faces");
    check(mirrored->subdivision && mirrored->subdivision->edgeCreases.size() == 2, "the crease is copied to the mirrored edge");
    check(mirrored->subdivision && evaluate_subdivision(*mirrored->polygon, *mirrored->subdivision).mesh.has_value(), "the cage subdivides");
    check(watertight(*mirrored->polygon), "the cage is closed");
}

void test_live_mirror() {
    auto owner = make_controller(false);
    NativeEditorController& c = *owner;
    EditorDocument& doc = c.workspace().document();
    const std::size_t plain = c.polygon_draw_items().size();
    check(c.dispatch_action("mirror.live_x") && c.live_mirror_axis(1) == 0, "Live Mirror X turns on");
    const MenuAction* item = std::as_const(c.workspace().menus()).find("mirror.live_x");
    check(item && item->checked, "the menu checks it");
    check(c.polygon_draw_items().size() > plain, "the other half is drawn");
    check(doc.find_object(1)->polygon->indices.size() == 36, "the mesh itself is unchanged");
    check(c.dispatch_action("mirror.live_y") && c.live_mirror_axis(1) == 1, "switch to Y");
    check(c.dispatch_action("mirror.live_x") && c.live_mirror_axis(1) == 0, "back to X");
    check(c.dispatch_action("mirror.apply_live"), "Apply Live Mirror: " + c.status().text);
    check(!c.live_mirror_axis(1) && watertight(*doc.find_object(1)->polygon) && doc.find_object(1)->polygon->indices.size() == 60,
          "applied: welded closed mesh, live mirror off");
    check(c.workspace().commands().undo(doc).success && c.live_mirror_axis(1) == 0 && doc.find_object(1)->polygon->indices.size() == 36,
          "undo brings back the half and the live mirror");
    check(c.dispatch_action("mirror.live_x") && !c.live_mirror_axis(1), "toggling the same axis turns it off");
    check(!c.dispatch_action("mirror.apply_live"), "nothing to apply without a live mirror");
}
}  // namespace

int main() {
    test_core();
    test_mirror_mesh_command();
    test_cage_and_creases();
    test_live_mirror();
    if (g_failures == 0) std::printf("mesh mirror tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
