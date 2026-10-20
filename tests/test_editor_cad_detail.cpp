// CAD Detail (exact CAD, first batch): re-mesh CAD objects from their exact source at a chosen
// accuracy; the accuracy is recorded on the exact source component and shown in the inspector.
//
// Always run, against a stand-in brepkernel: the library call (argv, report, errors), the stored
// accuracy and its world value, and the native command (presets, typed values, one undo step,
// materials kept, refusals). With the real brepkernel installed: a cylinder re-meshed finer has
// more triangles and the recorded accuracy matches.
#include "dve/editor_exact_cad.hpp"
#include "dve/editor_native.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "exact_cad_test_support.hpp"

namespace {
using namespace dve;
using namespace dve::editor;
using namespace exact_cad_test;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

void test_library(const fs::path& root, const fs::path& glb) {
    fs::create_directories(root / "sources/exact");
    write_text(root / "sources/exact/part-0123456789ab.brep", "DBRep_DrawableShape");
    ExactBrepSource source{"sources/exact/part-0123456789ab.brep", "0123456789ab", "part.step", 0.0, 0.0};
    std::vector<std::string> seen;
    ExactCadSettings settings;
    settings.command = {"brepkernel"};
    const ExactCadRunner runner = [&](const std::vector<std::string>& argv, std::chrono::seconds, std::uint64_t) {
        seen = argv;
        ExactCadProcessResult result;
        result.launched = true;
        std::string out, report;
        for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
            if (argv[i] == "-o") out = argv[i + 1];
            if (argv[i] == "--report") report = argv[i + 1];
        }
        fs::copy_file(glb, out);
        write_text(report, R"({"schema":"brepkernel.cli/1","status":"ok","mesh":{"linear_deflection":0.0001,"angular_deflection":0.35,"triangles":12}})");
        result.exitCode = 0;
        return result;
    };
    const auto remeshed = remesh_exact_cad(root, source, 7, 0.0001, settings, runner);
    check(bool(remeshed), "re-mesh with a fake tool: " + remeshed.error);
    check(seen.size() > 5 && seen[1] == "mesh" && seen[2] == (root / source.relativePath).string(), "brepkernel mesh <source>");
    bool deflection = false;
    for (std::size_t i = 0; i + 1 < seen.size(); ++i) deflection = deflection || (seen[i] == "--deflection" && std::stod(seen[i + 1]) == 0.0001);
    check(deflection, "the accuracy is passed as --deflection");
    check(remeshed.deflectionMeters == 0.0001 && remeshed.triangles == 12 && remeshed.mesh.indices.size() == 36, "report read back");
    check(remeshed.mesh.objectId == 7, "mesh belongs to the object");

    ExactBrepSource missing = source;
    missing.relativePath = "sources/exact/gone.brep";
    check(!remesh_exact_cad(root, missing, 7, 0.0001, settings, runner), "a missing source is refused");
    missing.relativePath = "../outside.brep";
    check(!remesh_exact_cad(root, missing, 7, 0.0001, settings, runner), "a source outside the project is refused");

    // Stored on the component; the world value follows the object's largest axis scale.
    source.deflectionMeters = 0.0002;
    source.angularRadians = 0.35;
    EditorObject object(3, "Part");
    object.polygon = make_editor_polygon_cube(3);
    Component component = make_exact_brep_component(source);
    component.id = 1;
    object.components.push_back(component);
    const auto back = exact_brep_source(object);
    check(back && back->deflectionMeters == 0.0002 && back->angularRadians == 0.35, "accuracy round-trips through the component");
    object.voxelScale = {1.0F, 3.0F, 2.0F};
    const auto world = exact_mesh_world_error(object);
    check(world && std::abs(*world - 0.0006) < 1.0e-12, "world error = deflection x largest scale");
    object.components[0].properties.erase("deflection");
    check(!exact_mesh_world_error(object), "unknown when not recorded");
}

std::unique_ptr<NativeEditorController> make_controller(const fs::path& root) {
    auto c = std::make_unique<NativeEditorController>();
    c->configure_ai_assistant(root);
    c->resize(1600, 900);
    EditorObject part(c->workspace().document().allocate_object_id(), "Bracket");
    part.polygon = make_editor_polygon_cube(part.id);
    part.polygon->materials.push_back({});
    part.polygon->materials.back().name = "Anodised";
    part.polygon->materialBindings.resize(part.polygon->materials.size());
    ExactBrepSource source{"sources/exact/part-0123456789ab.brep", "0123456789ab", "bracket.step", 0.0, 0.0};
    Component component = make_exact_brep_component(source);
    component.id = 1;
    part.components.push_back(component);
    const EditorObjectId id = part.id;
    c->workspace().document().add_object(std::move(part));
    c->workspace().select_object(id);
    return c;
}

void test_native(const fs::path& root, const fs::path& glb) {
    const fs::path script = write_fake_brepkernel(root, glb);
    const std::string command = "/bin/sh " + script.string();
    set_test_env("DVE_BREPKERNEL", command.c_str());
    auto owner = make_controller(root);
    NativeEditorController& c = *owner;
    const EditorObjectId id = *c.workspace().selected_object();
    check(c.cad_detail_summary(id).find("default mesh detail") != std::string::npos, "before: " + c.cad_detail_summary(id));
    c.refresh_menu_state();
    const MenuAction* action = std::as_const(c.workspace().menus()).find("polygon.cad_detail");
    check(action && action->enabled, "CAD Detail is enabled for a CAD object");
    check(c.dispatch_action("polygon.cad_detail") && c.context_menu().open && c.context_menu().items.size() == 6, "the accuracy list opens");
    const std::size_t steps = c.workspace().commands().size();
    check(c.dispatch_action("cad.detail.100"), "Medium: 0.1 mm: " + c.status().text);
    (void)c.wait_for_long_job(60.0);
    const EditorObject* part = c.workspace().document().find_object(id);
    check(part->polygon->indices.size() == 36, "re-meshed (the stand-in's cube)");
    check(c.cad_detail_summary(id) == "Exact CAD: mesh within \xC2\xB1" "0.1 mm", "after: " + c.cad_detail_summary(id));
    check(!part->polygon->materials.empty() && part->polygon->materials.back().name == "Anodised", "materials are kept");
    check(c.workspace().commands().size() == steps + 1, "one undo step");
    check(read_text(root / "log.txt").find("--deflection 0.0001 ") != std::string::npos || read_text(root / "log.txt").find("--deflection 1e-04 ") != std::string::npos, "0.1 mm in the mesh frame");
    check(c.dispatch_action("edit.undo") && c.cad_detail_summary(id).find("default") != std::string::npos, "undo restores the old detail");

    // A stretched object: the mesh-frame accuracy is the world accuracy over the largest scale.
    c.workspace().document().find_object(id)->voxelScale = {4.0F, 1.0F, 1.0F};
    check(c.dispatch_action("cad.detail.custom") && c.text_edit().kind == TextEditKind::CadDetail, "Custom opens the field");
    c.text_input("20um");
    c.key_down("enter", false, false, false);
    (void)c.wait_for_long_job(60.0);
    check(read_text(root / "log.txt").find("--deflection 5e-06 ") != std::string::npos, "20 µm in the world at 4x scale: " + read_text(root / "log.txt"));
    check(c.cad_detail_summary(id) == "Exact CAD: mesh within \xC2\xB1" "0.02 mm", "world accuracy shown: " + c.cad_detail_summary(id));
    check(c.dispatch_action("cad.detail.custom"), "custom again");
    c.text_input("0.5");
    c.key_down("enter", false, false, false);
    (void)c.wait_for_long_job(60.0);
    check(c.cad_detail_summary(id) == "Exact CAD: mesh within \xC2\xB1" "0.5 mm", "plain numbers are millimetres: " + c.cad_detail_summary(id));

    // Asking for the accuracy it already has adds no undo step.
    {
        const std::size_t before = c.workspace().commands().size();
        check(c.dispatch_action("cad.detail.custom"), "custom once more");
        c.text_input("0.5");
        c.key_down("enter", false, false, false);
        (void)c.wait_for_long_job(60.0);
        check(c.workspace().commands().size() == before, "the same accuracy again is a no-op: " + c.status().text);
    }
    // Trailing text around a micrometre value is an error, not a silent 20 µm.
    for (const char* bad : {"20dummy", "0.1mm+20um", "abcum"}) {
        const std::size_t before = c.workspace().commands().size();
        check(c.dispatch_action("cad.detail.custom"), "custom");
        c.text_input(bad);
        c.key_down("enter", false, false, false);
        (void)c.wait_for_long_job(60.0);
        check(c.status().error && c.workspace().commands().size() == before &&
              c.cad_detail_summary(id) == "Exact CAD: mesh within \xC2\xB1" "0.5 mm", std::string("refused: ") + bad + ": " + c.status().text);
    }

    // Refusals.
    c.workspace().document().find_object(id)->flags.locked = true;
    check(!c.dispatch_action("cad.detail.100") && c.status().text.find("Unlock") != std::string::npos, "locked objects are refused");
    c.refresh_menu_state();
    {
        const MenuAction* action = std::as_const(c.workspace().menus()).find("polygon.cad_detail");
        check(action && !action->enabled && action->disabledReason.find("Unlock") != std::string::npos, "the menu says why");
    }
    c.workspace().document().find_object(id)->flags.locked = false;
    EditorObject plain(c.workspace().document().allocate_object_id(), "Plain");
    plain.polygon = make_editor_polygon_cube(plain.id);
    const EditorObjectId plainId = plain.id;
    c.workspace().document().add_object(std::move(plain));
    c.workspace().select_object(plainId);
    c.refresh_menu_state();
    action = std::as_const(c.workspace().menus()).find("polygon.cad_detail");
    check(action && !action->enabled && action->disabledReason.find("CAD object") != std::string::npos, "plain meshes: disabled with a reason");
    check(c.cad_detail_summary(plainId).empty(), "no CAD line for plain meshes");
    unset_test_env("DVE_BREPKERNEL");
}

// With brepkernel installed: a cylinder re-meshed to 0.01 mm has more triangles than the default.
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
    check(imported.source.deflectionMeters > 0.0, "import records its accuracy");
    const auto fine = remesh_exact_cad(root, imported.source, 1, 1.0e-5, settings);
    check(bool(fine), "real re-mesh: " + fine.error);
    check(fine.deflectionMeters == 1.0e-5 && fine.mesh.indices.size() > imported.mesh.indices.size(),
          "finer accuracy, more triangles: " + std::to_string(fine.mesh.indices.size() / 3) + " vs " +
          std::to_string(imported.mesh.indices.size() / 3));
    // How close is it: every vertex lies on the cylinder (radius 6 mm or on a cap).
    double worst = 0.0;
    for (const auto& v : fine.mesh.vertices) {
        const double r = std::hypot(double(v.position.x), double(v.position.z));
        const bool cap = std::abs(std::abs(double(v.position.y)) - 0.01) < 1.0e-9;
        if (!cap) worst = std::max(worst, std::abs(r - 0.006));
    }
    check(worst < 1.0e-7, "side vertices lie on the exact radius");
    std::printf("real brepkernel: default %zu triangles (±%.3g mm), 0.01 mm %zu triangles\n", imported.mesh.indices.size() / 3,
                imported.source.deflectionMeters * 1000.0, fine.mesh.indices.size() / 3);
}
}  // namespace

int main() {
    TempDir dir;
    const fs::path glb = dir.path / "cube.glb";
    write_cube_glb(glb);
    test_library(dir.path, glb);
    if (fake_brepkernel_runs()) test_native(dir.path, glb);
    else std::printf("SKIP native CAD Detail: the fake brepkernel needs /bin/sh\n");
    test_real(dir.path);
    if (g_failures == 0) std::printf("cad detail tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
