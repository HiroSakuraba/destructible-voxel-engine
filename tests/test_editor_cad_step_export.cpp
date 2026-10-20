// File > Export Exact CAD (STEP) (exact CAD, first batch).
//
// Always run, against a stand-in brepkernel: world placements, the parts list handed to brepkernel
// (names, exact sources, 3x4 transforms), units and up axis, the report (converted stretched
// parts), refusals, an older brepkernel, and the native command (format list, groups, skipped
// plain objects, the status line, no file on failure). With the real brepkernel: a placed 12 mm
// pin and a stretched one written to STEP in mm, Z up, read back by brepkernel.
#include "dve/editor_exact_cad.hpp"
#include "dve/editor_native.hpp"
#include "dve/ai/json.hpp"

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

const char* kExportReport = R"({"parts":[{"name":"Bracket","placement":"rigid","exact":true,"volume_mm3":1000.0},
  {"name":"Stretched","placement":"general_affine","exact":false,"volume_mm3":2000.0}],
  "units":"mm","up":"z","solids":2,"readback":{"solids":2,"volume_mm3":3000.0,"relative_error":1e-12}})";

const ExactBrepSource kSource{"sources/exact/part-0123456789ab.brep", "0123456789ab", "part.step", 0.0, 0.0};

void test_placement() {
    EditorDocument document;
    EditorObject object(document.allocate_object_id(), "Part");
    object.transform.position = {1.0F, 2.0F, 3.0F};
    object.transform.rotation = quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, 1.5707963267948966F);
    object.voxelScale = {2.0F, 2.0F, 2.0F};
    const EditorObjectId id = object.id;
    document.add_object(std::move(object));
    const auto m = exact_world_placement(document, id);
    check(m.has_value(), "placement");
    if (!m) return;
    // +X (local) -> -Z (world) under a +90 degree turn about Y, times 2.
    check(std::abs((*m)[0]) < 1e-6 && std::abs((*m)[8] + 2.0) < 1e-6 && std::abs((*m)[5] - 2.0) < 1e-6, "rotation x scale");
    check((*m)[3] == 1.0 && (*m)[7] == 2.0 && (*m)[11] == 3.0, "translation");
    document.find_object(id)->voxelScale = {0.0F, 1.0F, 1.0F};
    check(!exact_world_placement(document, id), "a zero scale has no placement");
}

void test_library(const fs::path& root, const fs::path& glb) {
    fs::create_directories(root / "sources/exact");
    write_text(root / "sources/exact/part-0123456789ab.brep", "DBRep_DrawableShape");
    const fs::path script = write_fake_brepkernel(root, glb, "{}", "[]", kExportReport);
    ExactCadSettings settings;
    settings.command = {"/bin/sh", script.string()};
    std::array<double, 12> moved{1, 0, 0, 0.25, 0, 1, 0, 0, 0, 0, 1, 0};
    std::array<double, 12> stretched{2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const std::vector<ExactStepPart> parts{{kSource, "Bracket", moved}, {kSource, "Stretched", stretched}};
    const auto out = root / "exports/parts.step";
    const auto result = export_exact_cad_step(root, parts, out, true, true, settings);
    check(bool(result), "export: " + result.error);
    check(result.parts.size() == 2 && result.parts[0].exact && !result.parts[1].exact &&
              result.parts[1].placement == "general_affine" && result.parts[1].volumeMm3 == 2000.0,
          "report read back");
    check(has(read_text(root / "log.txt"), "-o " + out.string() + " --units mm --up z"), "mm, Z up passed on");
    const auto manifest = dve::ai::parse_json(read_text(root / "parts_seen.json"));
    check(manifest.value && manifest.value->find("parts") && manifest.value->find("parts")->as_array().size() == 2, "parts list written");
    if (manifest.value && manifest.value->find("parts")) {
        const auto& first = manifest.value->find("parts")->as_array()[0];
        check(first.find("name")->as_string() == "Bracket" && first.find("path")->as_string() == (root / kSource.relativePath).string(),
              "named part, project source");
        check(first.find("transform")->as_array().size() == 12 && first.find("transform")->as_array()[3].as_number() == 0.25,
              "3x4 transform");
    }
    check(bool(export_exact_cad_step(root, parts, root / "exports/m.step", false, false, settings)) &&
              has(read_text(root / "log.txt"), "--units m --up y"),
          "metres, Y up");

    check(!export_exact_cad_step(root, {}, out, true, true, settings), "nothing to export: refused");
    check(has(export_exact_cad_step(root, parts, root / "exports/parts.stl", true, true, settings).error, ".step"), "wrong extension refused");
    ExactStepPart gone{kSource, "Gone", moved};
    gone.source.relativePath = "sources/exact/gone.brep";
    const auto missing = export_exact_cad_step(root, {gone}, out, true, true, settings);
    check(!missing && has(missing.error, "Gone"), "missing source named: " + missing.error);

    const fs::path old = root / "old_brepkernel.sh";
    write_text(old, "#!/bin/sh\necho \"brepkernel: error: argument command: invalid choice: '$1'\" >&2\nexit 2\n");
    ExactCadSettings oldSettings;
    oldSettings.command = {"/bin/sh", old.string()};
    const auto o = export_exact_cad_step(root, parts, out, true, true, oldSettings);
    check(!o && o.unsupported && has(o.error, "update brepkernel"), "older brepkernel: " + o.error);
}

void test_native(const fs::path& root) {
    setenv("DVE_BREPKERNEL", ("/bin/sh " + (root / "fake_brepkernel.sh").string()).c_str(), 1);
    auto owner = std::make_unique<NativeEditorController>();
    NativeEditorController& c = *owner;
    c.configure_ai_assistant(root);
    c.resize(1600, 900);
    EditorDocument& doc = c.workspace().document();
    const auto add = [&](const char* name, bool cad, Float3 scale) {
        EditorObject object(doc.allocate_object_id(), name);
        object.polygon = make_editor_polygon_cube(object.id);
        object.voxelScale = scale;
        if (cad) {
            Component component = make_exact_brep_component(kSource);
            component.id = 1;
            object.components.push_back(component);
        }
        const EditorObjectId id = object.id;
        doc.add_object(std::move(object));
        return id;
    };
    const EditorObjectId bracket = add("Bracket", true, {1, 1, 1});
    const EditorObjectId stretched = add("Stretched", true, {2, 1, 1});
    const EditorObjectId lamp = add("Lamp", false, {1, 1, 1});

    c.workspace().select_object(lamp);
    c.refresh_menu_state();
    const MenuAction* action = std::as_const(c.workspace().menus()).find("file.export_exact_step");
    check(action && !action->enabled && has(action->disabledReason, "CAD"), "disabled without CAD objects");
    check(!c.dispatch_action("file.export_exact_step"), "refused without CAD objects");

    c.workspace().select_object(bracket);
    c.workspace().add_to_selection(stretched);
    c.workspace().add_to_selection(lamp);
    c.refresh_menu_state();
    action = std::as_const(c.workspace().menus()).find("file.export_exact_step");
    check(action && action->enabled, "enabled with CAD objects selected");
    check(c.dispatch_action("file.export_exact_step") && c.context_menu().open && c.context_menu().items.size() == 4, "format list");
    check(c.dispatch_action("cad.step.mm_z"), "export: " + c.status().text);
    (void)c.wait_for_long_job(60.0);
    const std::string status = c.status().text;
    check(has(status, "Exported 2 CAD parts to exports/") && has(status, "(mm, Z up), read back and checked"), "status: " + status);
    check(has(status, "exported converted (not exact): Stretched") && has(status, "skipped, no exact source: Lamp"), "named: " + status);
    check(c.last_step_export().extension() == ".step", "remembered: " + c.last_step_export().generic_string());
    check(fs::exists(root / c.last_step_export()), "the file is there");
    const auto manifest = dve::ai::parse_json(read_text(root / "parts_seen.json"));
    check(manifest.value && manifest.value->find("parts")->as_array().size() == 2 &&
              manifest.value->find("parts")->as_array()[1].find("transform")->as_array()[0].as_number() == 2.0,
          "the stretched object's world scale goes to brepkernel");

    // Failure: no file.
    write_text(root / "fake_brepkernel.sh", "#!/bin/sh\nwhile [ $# -gt 0 ]; do case \"$1\" in --report) report=\"$2\"; shift;; esac; shift; done\n"
               "printf '{\"schema\":\"brepkernel.cli/1\",\"status\":\"error\",\"error\":\"read-back check failed\"}' > \"$report\"; exit 1\n");
    fs::remove(root / c.last_step_export());
    check(c.dispatch_action("cad.step.mm_z"), "second export starts");
    (void)c.wait_for_long_job(60.0);
    check(has(c.status().text, "no file written") && has(c.status().text, "read-back check failed"), "failure reported: " + c.status().text);
    check(!fs::exists(root / "exports/Bracket.step"), "no file after a failure");
    unsetenv("DVE_BREPKERNEL");
}

void test_real(const fs::path& root) {
    const auto settings = ExactCadSettings::from_environment();
    const auto probe = run_exact_cad_process(
        [&] { auto argv = settings.command; argv.insert(argv.end(), {"primitive", "cylinder", "-o", (root / "pin.brep").string(),
              "--radius", "0.006", "--height", "0.02", "--report", (root / "p.json").string()}); return argv; }(),
        std::chrono::seconds(120), 1U << 20U);
    if (!probe.launched || probe.exitCode != 0) {
        std::printf("SKIP real brepkernel: not available\n");
        if (std::getenv("DVE_REQUIRE_BREPKERNEL")) check(false, "brepkernel required");
        return;
    }
    fs::create_directories(root / "assets");
    const auto imported = import_exact_cad(root, root / "pin.brep", 1, settings);
    check(bool(imported), "real import: " + imported.error);
    if (!imported) return;
    const std::vector<ExactStepPart> parts{
        {imported.source, "Pin", {1, 0, 0, 0.05, 0, 1, 0, 0, 0, 0, 1, 0}},
        {imported.source, "Wide pin", {2, 0, 0, -0.05, 0, 1, 0, 0, 0, 0, 1, 0}}};
    const auto out = root / "exports/pins.step";
    const auto result = export_exact_cad_step(root, parts, out, true, true, settings);
    if (result.unsupported) { std::printf("SKIP real export: brepkernel predates export\n"); return; }
    check(bool(result), "real export: " + result.error);
    check(result.parts.size() == 2 && result.parts[0].exact && !result.parts[1].exact, "the stretched pin is converted");
    const double pin = 3.14159265358979 * 6.0 * 6.0 * 20.0;   // mm^3
    if (result.parts.size() == 2) {
        check(std::abs(result.parts[0].volumeMm3 - pin) < pin * 1e-6, "pin volume in mm^3: " + std::to_string(result.parts[0].volumeMm3));
        check(std::abs(result.parts[1].volumeMm3 - 2.0 * pin) < pin * 1e-5, "stretched volume doubles");
    }
    check(result.readbackRelativeError < 1e-6, "read back");
    const std::string text = read_text(out);
    check(has(text, "ISO-10303-21") && has(text, "'Pin'") && has(text, "'Wide pin'") && has(text, "MILLI"), "named STEP in mm");
    // A failing export (a part path that is not a B-rep) leaves the earlier file as it was.
    write_text(root / "sources/exact/broken-000000000000.brep", "not a brep");
    ExactStepPart broken{imported.source, "Broken", parts[0].placement};
    broken.source.relativePath = "sources/exact/broken-000000000000.brep";
    const auto failed = export_exact_cad_step(root, {broken}, out, true, true, settings);
    check(!failed && read_text(out) == text, "a failed export keeps the old file: " + failed.error);
    std::printf("real STEP export: %zu bytes, read-back error %.3g\n", text.size(), result.readbackRelativeError);
}
}  // namespace

int main() {
    TempDir dir;
    const fs::path glb = dir.path / "cube.glb";
    write_cube_glb(glb);
    test_placement();
    test_library(dir.path, glb);
    test_native(dir.path);
    test_real(dir.path);
    if (g_failures == 0) std::printf("cad step export tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
