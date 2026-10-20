// View > Measure and exact CAD snapping (exact CAD, first batch).
//
// Always run, against a stand-in brepkernel: describe / query parsing, face names, the message
// for a brepkernel without these commands, exact snap targets (corners, circle centres) replacing
// the triangle ones, two-point distances in mm for CAD and metres otherwise, and a CAD surface
// click projected onto the exact surface and named. With the real brepkernel: a 12 mm cylinder's
// circles, faces and closest points.
#include "dve/editor_exact_cad.hpp"
#include "dve/editor_native.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "exact_cad_test_support.hpp"

namespace {
using namespace dve;
using namespace dve::editor;
using namespace exact_cad_test;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A unit cube (0..1) with a 0.6 m circle on its top face and a 12 mm hole face first.
const char* kDescribe = R"({"solids":1,"volume":1.0,"area":6.0,
 "faces":[{"index":0,"area":0.1,"reversed":true,"type":"cylinder","axis_origin":[0.5,0,0.5],"axis":[0,1,0],"radius":0.006,"hole":true},
          {"index":1,"area":1,"reversed":false,"type":"plane","origin":[0,1,0],"normal":[0,1,0]},
          {"index":2,"area":1,"type":"sphere","centre":[0,0,0],"radius":0.01},
          {"index":3,"area":1,"type":"bspline","u_degree":3,"v_degree":3}],
 "vertices":[[0,0,0],[1,0,0],[1,1,0],[0,1,0],[0,0,1],[1,0,1],[1,1,1],[0,1,1]],
 "edges":[{"type":"line","start":[0,1,0],"end":[1,1,0],"mid":[0.5,1,0],"length":1.0}],
 "circles":[{"centre":[0.5,1,0.5],"axis":[0,1,0],"radius":0.3,"full":true}]})";
const char* kQuery = R"([{"point":[0,0,0],"found":true,"closest":[0.25,1.0,0.25],"distance":0.0001,"face":0,"normal":[0,1,0],"uv":[0,0]}])";

void test_library(const fs::path& root, const fs::path& glb) {
    fs::create_directories(root / "sources/exact");
    write_text(root / "sources/exact/part-0123456789ab.brep", "DBRep_DrawableShape");
    const fs::path script = write_fake_brepkernel(root, glb, kDescribe, kQuery);
    ExactCadSettings settings;
    settings.command = {"/bin/sh", script.string()};
    const ExactBrepSource source{"sources/exact/part-0123456789ab.brep", "0123456789ab", "part.step", 0.0, 0.0};
    const auto d = describe_exact_cad(root, source, settings);
    check(bool(d), "describe: " + d.error);
    check(d.faces.size() == 4 && d.faces[0].type == "cylinder" && d.faces[0].hole && d.faces[0].radius == 0.006, "faces parsed");
    check(d.vertices.size() == 8 && d.vertices[6][0] == 1.0 && d.edges.size() == 1 && d.edges[0].mid[0] == 0.5, "vertices and edges");
    check(d.circles.size() == 1 && d.circles[0].full && d.circles[0].radius == 0.3, "circles");
    check(describe_exact_face(d.faces[0]) == "Hole \xC3\x98 12.000 mm", "hole name: " + describe_exact_face(d.faces[0]));
    check(describe_exact_face(d.faces[0], 2.0) == "Hole \xC3\x98 24.000 mm", "scaled object");
    check(describe_exact_face(d.faces[1]) == "Plane" && describe_exact_face(d.faces[2]) == "Sphere \xC3\x98 20.000 mm" &&
              describe_exact_face(d.faces[3]) == "Freeform surface (NURBS)", "other faces");
    const auto q = query_exact_cad(root, source, {{0.25, 1.01, 0.25}}, settings);
    check(bool(q) && q.hits.size() == 1 && q.hits[0].found && q.hits[0].face == 0 && q.hits[0].closest[1] == 1.0, "query: " + q.error);
    check(has(read_text(root / "log.txt"), "query " + (root / source.relativePath).string() + " --point 0.25 1.01 0.25"), "query argv");
    check(!query_exact_cad(root, source, {}, settings), "no points: refused");
    ExactBrepSource outside = source;
    outside.relativePath = "../x.brep";
    check(!describe_exact_cad(root, outside, settings), "outside the project: refused");

    // An older brepkernel (argparse rejects the command, no report).
    const fs::path old = root / "old_brepkernel.sh";
    write_text(old, "#!/bin/sh\necho \"brepkernel: error: argument command: invalid choice: '$1'\" >&2\nexit 2\n");
    ExactCadSettings oldSettings;
    oldSettings.command = {"/bin/sh", old.string()};
    const auto o = describe_exact_cad(root, source, oldSettings);
    check(!o && o.unsupported && has(o.error, "update brepkernel"), "old brepkernel: " + o.error);
}

std::unique_ptr<NativeEditorController> make_controller(const fs::path& root, EditorObjectId& cad, EditorObjectId& plain) {
    auto c = std::make_unique<NativeEditorController>();
    c->configure_ai_assistant(root);
    c->resize(1600, 900);
    EditorDocument& doc = c->workspace().document();
    EditorObject part(doc.allocate_object_id(), "Plate");
    part.polygon = make_editor_polygon_cube(part.id);
    Component component = make_exact_brep_component({"sources/exact/part-0123456789ab.brep", "0123456789ab", "plate.step", 0.0, 0.0});
    component.id = 1;
    part.components.push_back(component);
    cad = part.id;
    doc.add_object(std::move(part));
    EditorObject block(doc.allocate_object_id(), "Block");
    block.polygon = make_editor_polygon_cube(block.id);
    block.transform.position = {3.0F, 0.0F, 0.0F};
    plain = block.id;
    doc.add_object(std::move(block));
    c->workspace().select_object(cad);
    (void)c->dispatch_action("view.frame");
    c->workspace().clear_selection();
    return c;
}

std::pair<int, int> screen(const NativeEditorController& c, Float3 world) {
    const ScreenPoint p = project_world_to_screen(c.camera(), c.layout().viewport, world);
    return {static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y))};
}

void test_native(const fs::path& root) {
    setenv("DVE_BREPKERNEL", ("/bin/sh " + (root / "fake_brepkernel.sh").string()).c_str(), 1);
    EditorObjectId cad{}, plain{};
    auto owner = make_controller(root, cad, plain);
    NativeEditorController& c = *owner;

    // Before the exact shape is read, the top-face circle centre is not a target.
    auto [cx, cy] = screen(c, {0.5F, 1.0F, 0.5F});
    auto target = c.find_snap_target(cx, cy, std::nullopt, true);
    check(!target || target->kind != SnapTargetKind::CircleCentre, "no circle centre before describe");
    c.update(0.016F);
    check(c.wait_for_cad_descriptions(30.0) && c.exact_cad_description(cad) != nullptr, "the exact shape is read in the background");
    check(c.exact_cad_description(plain) == nullptr, "plain meshes have no exact shape");
    target = c.find_snap_target(cx, cy, std::nullopt, true);
    check(target && target->kind == SnapTargetKind::CircleCentre && target->object == cad &&
              length(subtract(target->point, Float3{0.5F, 1.0F, 0.5F})) < 1.0e-5F,
          "snaps to the exact circle centre");
    // The exact edge midpoint (0.5, 1, 0) and corner (1, 1, 1); triangle-only midpoints are gone.
    auto [ex, ey] = screen(c, {0.5F, 1.0F, 0.0F});
    target = c.find_snap_target(ex, ey, SnapTargetKind::EdgeMidpoint, true);
    check(target && length(subtract(target->point, Float3{0.5F, 1.0F, 0.0F})) < 1.0e-5F, "exact edge midpoint");
    auto [dx, dy] = screen(c, {0.5F, 1.0F, 1.0F});   // a cube edge midpoint, not in the exact edge list
    target = c.find_snap_target(dx, dy, SnapTargetKind::EdgeMidpoint, true);
    check(!target || length(subtract(target->point, Float3{0.5F, 1.0F, 1.0F})) > 1.0e-3F, "triangle midpoints are not used for CAD objects");

    // Measure: circle centre to corner, in millimetres, exact.
    check(c.dispatch_action("view.measure") && c.measuring(), "View > Measure");
    c.refresh_menu_state();
    const MenuAction* action = std::as_const(c.workspace().menus()).find("view.measure");
    check(action && action->checked, "Measure is checked in the View menu");
    check(c.measure_at(cx, cy) && c.measure_points().size() == 1 && c.measure_points()[0].exact, "first point");
    check(has(c.measure_points()[0].what, "circle centre \xC3\x98 600.000 mm"), "names the circle: " + c.measure_points()[0].what);
    auto [vx, vy] = screen(c, {1.0F, 1.0F, 1.0F});
    check(c.measure_at(vx, vy) && c.measure_points().size() == 2, "second point");
    const std::string readout = c.measure_readout();
    check(has(readout, "Distance 707.107 mm") && has(readout, "X 500.000 mm") && has(readout, "Y 0.000 mm") && has(readout, "exact"),
          "CAD distance in mm: " + readout);
    check(c.status().text == readout, "shown in the status bar");

    // A third click starts again; a CAD surface click is projected onto the exact surface.
    auto [sx, sy] = screen(c, {0.2F, 1.0F, 0.8F});   // away from corners and the circle centre
    check(c.measure_at(sx, sy) && c.measure_points().size() == 1, "third click starts over");
    check(c.wait_for_long_job(60.0), "exact surface look-up finishes");
    const NativeMeasurePoint& p = c.measure_points().front();
    check(p.exact && p.what == "Hole \xC3\x98 12.000 mm", "face named: " + p.what + " / " + c.status().text);
    check(length(subtract(p.world, Float3{0.25F, 1.0F, 0.25F})) < 1.0e-5F, "point moved to the exact closest point");

    check(c.measure_at(vx, vy) && has(c.measure_readout(), "Distance 1060.660 mm"), "hole face to corner: " + c.measure_readout());
    c.set_measuring(false);
    check(c.measure_points().empty(), "points cleared");
    (void)c.measure_at(vx, vy);
    check(c.measure_points().empty(), "clicks do nothing when not measuring");
    unsetenv("DVE_BREPKERNEL");
}

void test_plain_only() {
    auto c = std::make_unique<NativeEditorController>();
    c->resize(1600, 900);
    EditorObject block(c->workspace().document().allocate_object_id(), "Block");
    block.polygon = make_editor_polygon_cube(block.id, 2.0F);
    const EditorObjectId id = block.id;
    c->workspace().document().add_object(std::move(block));
    c->workspace().select_object(id);
    (void)c->dispatch_action("view.frame");
    c->set_measuring(true);
    const auto a = screen(*c, {0.0F, 2.0F, 0.0F});
    const auto b = screen(*c, {2.0F, 2.0F, 2.0F});
    check(c->measure_at(a.first, a.second) && c->measure_at(b.first, b.second), "two plain corners");
    check(has(c->measure_readout(), "Distance 2.828 m") && !has(c->measure_readout(), "mm"), "plain distance in metres: " + c->measure_readout());
    c->key_down("escape", false, false, false);
    check(!c->measuring(), "Esc ends Measure");

    // A new scene ends Measure: a point from the old scene is never measured against the new one.
    c->set_measuring(true);
    check(c->measure_at(a.first, a.second) && c->measure_points().size() == 1, "one point picked");
    c->workspace().preferences().confirmDestructiveActions = false;
    (void)c->dispatch_action("file.new_scene");
    if (c->pending_destructive_confirmation()) c->key_down("enter", false, false, false);
    check(!c->measuring() && c->measure_points().empty(), "New Scene ends Measure");
}

void test_real(const fs::path& root) {
    const auto settings = ExactCadSettings::from_environment();
    const auto probe = run_exact_cad_process(
        [&] { auto argv = settings.command; argv.insert(argv.end(), {"primitive", "cylinder", "-o", (root / "cyl.brep").string(),
              "--radius", "0.006", "--height", "0.02", "--report", (root / "p.json").string()}); return argv; }(),
        std::chrono::seconds(120), 1U << 20U);
    if (!probe.launched || probe.exitCode != 0) {
        std::printf("SKIP real brepkernel: not available\n");
        if (std::getenv("DVE_REQUIRE_BREPKERNEL")) check(false, "brepkernel required");
        return;
    }
    fs::create_directories(root / "assets");
    const auto imported = import_exact_cad(root, root / "cyl.brep", 1, settings);
    check(bool(imported), "real import: " + imported.error);
    if (!imported) return;
    const auto d = describe_exact_cad(root, imported.source, settings);
    if (d.unsupported) { std::printf("SKIP real describe: brepkernel predates describe\n"); return; }
    check(bool(d), "real describe: " + d.error);
    int full = 0;
    for (const auto& circle : d.circles) full += circle.full && std::abs(circle.radius - 0.006) < 1e-12 ? 1 : 0;
    check(full == 2, "two full 12 mm circles");
    const auto side = std::find_if(d.faces.begin(), d.faces.end(), [](const ExactCadFace& f) { return f.type == "cylinder"; });
    check(side != d.faces.end() && describe_exact_face(*side) == "Cylinder \xC3\x98 12.000 mm", "side face named");
    const auto q = query_exact_cad(root, imported.source, {{0.01, 0.0, 0.0}, {0.0, 0.02, 0.0}}, settings);
    check(bool(q) && q.hits.size() == 2, "real query: " + q.error);
    if (q && q.hits.size() == 2) {
        check(std::abs(q.hits[0].closest[0] - 0.006) < 1e-12 && std::abs(q.hits[0].distance - 0.004) < 1e-12, "closest point on the side");
        check(side != d.faces.end() && q.hits[0].face == side->index, "on the side face");
        check(std::abs(q.hits[1].closest[1] - 0.01) < 1e-12, "closest point on the top cap");
    }
    std::printf("real brepkernel: %zu faces, %zu edges, %zu circles\n", d.faces.size(), d.edges.size(), d.circles.size());

    // In the editor: the shape is read in the background and Measure snaps to the top circle centre.
    unsetenv("DVE_BREPKERNEL");
    auto c = std::make_unique<NativeEditorController>();
    c->configure_ai_assistant(root);
    c->resize(1600, 900);
    EditorObject part(c->workspace().document().allocate_object_id(), "Pin");
    part.polygon = imported.mesh;
    part.polygon->objectId = part.id;
    Component component = make_exact_brep_component(imported.source);
    component.id = 1;
    part.components.push_back(component);
    part.voxelScale = {10.0F, 10.0F, 10.0F};   // 120 mm across: big enough on screen at the closest framing
    const EditorObjectId id = part.id;
    c->workspace().document().add_object(std::move(part));
    c->workspace().select_object(id);
    (void)c->dispatch_action("view.frame");
    c->update(0.016F);
    check(c->wait_for_cad_descriptions(120.0) && c->exact_cad_description(id), "real describe in the background");
    c->set_measuring(true);
    const auto top = screen(*c, {0.0F, 0.1F, 0.0F});
    const auto rim = screen(*c, {0.0F, -0.1F, 0.06F});
    const bool picked = c->measure_at(top.first, top.second);
    check(picked && c->measure_points().back().kind == SnapTargetKind::CircleCentre &&
              has(c->measure_points().back().what, "\xC3\x98 120.000 mm"),
          "real: top circle centre: " + (c->measure_points().empty() ? std::string() : c->measure_points().back().what));
    check(c->measure_at(rim.first, rim.second), "real: second point");
    std::printf("real measure: %s\n", c->measure_readout().c_str());
}
}  // namespace

int main() {
    TempDir dir;
    const fs::path glb = dir.path / "cube.glb";
    write_cube_glb(glb);
    test_library(dir.path, glb);
    test_native(dir.path);
    test_plain_only();
    test_real(dir.path);
    if (g_failures == 0) std::printf("cad measure tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
