// Exact CAD source (dve.exact_brep) and the brepkernel bridge.
//
// Always run: the B-into-A placement against voxel_to_world/world_to_voxel (rotations, per-axis
// voxelScale, a parent attachment), the component's save/load round trip and its removal from
// runtime exports, and the import / Boolean flows against a fake brepkernel (argv, reports,
// refusals with advisory, errors, empty results, project-owned .brep storage).
//
// With brepkernel installed (found the same way the editor finds it: $DVE_BREPKERNEL, then
// `brepkernel` on PATH, then `python3 -m brepkernel`): a real import, a certified exact
// difference whose mesh volume matches the analytic one, a real refusal, and the native editor
// commands end to end. Without it those parts print SKIP; set DVE_REQUIRE_BREPKERNEL=1 to make
// a missing tool a failure.

#include "dve/editor_exact_cad.hpp"
#include "dve/collision_convex_set.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/runtime_scene.hpp"
#include "dve/voxel_metric.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::editor;
namespace fs = std::filesystem;

namespace {

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

double mesh_volume(const CookedPolygonAsset& m) {
    double out = 0;
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const auto a = m.vertices[m.indices[i]].position, b = m.vertices[m.indices[i + 1]].position,
                   c = m.vertices[m.indices[i + 2]].position;
        out += double(a.x) * (double(b.y) * c.z - double(b.z) * c.y) + double(a.y) * (double(b.z) * c.x - double(b.x) * c.z) +
               double(a.z) * (double(b.x) * c.y - double(b.y) * c.x);
    }
    return out / 6;
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
            ("dve-exact-cad-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Unit cube [0,1]^3 as a GLB: 24 vertices (hard edges), outward normals, CCW triangles.
void write_cube_glb(const fs::path& path) {
    struct V { float p[3], n[3]; };
    std::vector<V> vertices;
    std::vector<std::uint32_t> indices;
    const float faces[6][3][3] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
    for (const auto& f : faces) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const float su[4] = {-.5F, .5F, .5F, -.5F}, sv[4] = {-.5F, -.5F, .5F, .5F};
        for (int k = 0; k < 4; ++k) {
            V v{};
            for (int i = 0; i < 3; ++i) {
                v.p[i] = .5F + .5F * f[0][i] + su[k] * f[1][i] + sv[k] * f[2][i];
                v.n[i] = f[0][i];
            }
            vertices.push_back(v);
        }
        for (const std::uint32_t i : {0U, 1U, 2U, 0U, 2U, 3U}) indices.push_back(base + i);
    }
    std::string bin;
    for (const auto& v : vertices) bin.append(reinterpret_cast<const char*>(v.p), sizeof v.p);
    for (const auto& v : vertices) bin.append(reinterpret_cast<const char*>(v.n), sizeof v.n);
    bin.append(reinterpret_cast<const char*>(indices.data()), indices.size() * 4);
    std::ostringstream json;
    json << R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)" << bin.size()
         << R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":288},{"buffer":0,"byteOffset":288,"byteLength":288},)"
         << R"({"buffer":0,"byteOffset":576,"byteLength":144}],"accessors":[)"
         << R"({"bufferView":0,"componentType":5126,"count":24,"type":"VEC3","min":[0,0,0],"max":[1,1,1]},)"
         << R"({"bufferView":1,"componentType":5126,"count":24,"type":"VEC3"},)"
         << R"({"bufferView":2,"componentType":5125,"count":36,"type":"SCALAR"}],)"
         << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],)"
         << R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    std::string text = json.str();
    while (text.size() % 4) text += ' ';
    while (bin.size() % 4) bin += '\0';
    const auto u32 = [](std::string& out, std::uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    std::string glb;
    u32(glb, 0x46546C67U);
    u32(glb, 2);
    u32(glb, static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + bin.size()));
    u32(glb, static_cast<std::uint32_t>(text.size()));
    u32(glb, 0x4E4F534AU);
    glb += text;
    u32(glb, static_cast<std::uint32_t>(bin.size()));
    u32(glb, 0x004E4942U);
    glb += bin;
    write_text(path, glb);
}

// --- fake brepkernel ----------------------------------------------------------------------

struct Fake {
    enum class Mode { Ok, Refuse, Error, Empty, BadSchema, NoLaunch, Timeout };
    Mode mode{Mode::Ok};
    std::vector<std::string> argv;
    int calls{};
    bool piecesRefuse{};                  // convex-pieces answers "not an extrusion"
    std::vector<std::string> piecesArgv;
    int pieceCalls{};

    static std::string option(const std::vector<std::string>& argv, std::string_view name) {
        for (std::size_t i = 0; i + 1 < argv.size(); ++i)
            if (argv[i] == name) return argv[i + 1];
        return {};
    }
    [[nodiscard]] bool has(std::string_view flag) const {
        for (const auto& a : argv)
            if (a == flag) return true;
        return false;
    }

    ExactCadRunner runner() {
        return [this](const std::vector<std::string>& args, std::chrono::seconds, std::uint64_t) {
            if (std::find(args.begin(), args.end(), "convex-pieces") == args.end()) {
                argv = args;                  // convex-pieces calls are recorded on their own
                ++calls;
            }
            ExactCadProcessResult result;
            if (mode == Mode::NoLaunch) { result.error = "No such file or directory"; return result; }
            result.launched = true;
            if (mode == Mode::Timeout) { result.timedOut = true; return result; }
            const auto report = option(args, "--report");
            if (std::find(args.begin(), args.end(), "convex-pieces") != args.end()) {
                piecesArgv = args;
                ++pieceCalls;
                if (piecesRefuse) {
                    write_text(report, R"({"schema":"brepkernel.cli/1","status":"refused","certificate":{"status":"refused",)"
                                       R"("refusal":{"kind":"NotAnExtrusion","message":"the part is not a straight extrusion"}}})");
                    result.exitCode = 2;
                    return result;
                }
                // a 1 x 1 plate (y in [0, 0.1]) with a 0.4 x 0.4 square hole: four prisms
                std::string pieces = R"({"schema":"brepkernel.convex_pieces/1","units":"meters","pieces":[)";
                const double boxes[4][4] = {{0, 0.3, 0, 1}, {0.7, 1, 0, 1}, {0.3, 0.7, 0, 0.3}, {0.3, 0.7, 0.7, 1}};
                for (int k = 0; k < 4; ++k) {
                    const auto [x0, x1, z0, z1] = boxes[k];
                    std::ostringstream ring;
                    ring.imbue(std::locale::classic());
                    for (double y : {0.0, 0.1}) {
                        ring << (y == 0.0 ? R"("bottom":[)" : R"(],"top":[)");
                        ring << "[" << x0 << "," << y << "," << z0 << "],[" << x1 << "," << y << "," << z0 << "],["
                             << x1 << "," << y << "," << z1 << "],[" << x0 << "," << y << "," << z1 << "]";
                    }
                    pieces += std::string(k ? "," : "") + "{" + ring.str() + "]}";
                }
                write_text(option(args, "-o"), pieces + "]}");
                write_text(report, R"({"schema":"brepkernel.cli/1","status":"certified","certificate":{"status":"certified",)"
                                   R"("slabs":[{"deviation":{"reference_outside_sim":4.7e-05}}]}})");
                result.exitCode = 0;
                return result;
            }
            const bool boolean = std::find(args.begin(), args.end(), "boolean") != args.end();
            const std::string schema = mode == Mode::BadSchema ? "something/else" : "brepkernel.cli/1";
            if (mode == Mode::Error) {
                write_text(report, R"({"schema":")" + schema + R"(","status":"error","error":"file not found: x.step"})");
                result.exitCode = 1;
                return result;
            }
            if (mode == Mode::Refuse) {
                write_text(report, R"({"schema":")" + schema + R"(","status":"refused","refusal":{"stage":"verification",)"
                                   R"("kind":"IntersectionCompletenessProbeFailed","message":"a section branch is unaccounted for"},)"
                                   R"("advisory":{"certified":false,"engines":{"occt":{"status":"answered","volume":3.02661},)"
                                   R"("smlib":{"status":"answered","volume":3.01443}},"triage":"contested"}})");
                result.exitCode = 2;
                return result;
            }
            if (mode == Mode::Empty) {
                write_text(report, R"({"schema":")" + schema + R"(","status":"accepted_empty","result":{"volume":0,"solids":0}})");
                result.exitCode = 0;
                return result;
            }
            write_text(option(args, "-o"), boolean ? "fake result brep" : "fake imported brep");
            write_cube_glb(option(args, "--mesh"));
            if (boolean)
                write_text(report, R"({"schema":")" + schema + R"(","status":"accepted","result":{"sha256":"feedface00112233445566","volume":1.0},"mesh":{"triangles":12}})");
            else
                write_text(report, R"({"schema":")" + schema + R"(","status":"ok","input":{"format":"step"},"frame":{"scale":0.001,"up":"z"},)"
                                   R"("output":{"sha256":"abcdef0123456789abcdef"},"mesh":{"triangles":12}})");
            result.exitCode = 0;
            return result;
        };
    }
};

ExactCadSettings fake_settings() {
    ExactCadSettings settings;
    settings.command = {"brepkernel-fake"};
    return settings;
}

EditorObject exact_object(EditorObjectId id, std::string name, const fs::path& relative) {
    EditorObject object(id, std::move(name));
    object.polygon = make_editor_polygon_cube(id);
    Component component = make_exact_brep_component({relative, "0123456789abcdef", "test"});
    component.id = 1;
    object.components.push_back(std::move(component));
    return object;
}

// --- tests --------------------------------------------------------------------------------

void placement_matches_voxel_metric() {
    EditorDocument document("Placement");
    EditorObject parent(10, "Parent");
    parent.transform.position = {2, -1, .5F};
    parent.transform.rotation = quaternion_from_axis_angle({0, 1, 0}, .7F);
    document.add_object(std::move(parent));
    EditorObject a(1, "A");
    a.polygon = make_editor_polygon_cube(1);
    a.transform.position = {.3F, 1.2F, -.4F};
    a.transform.rotation = quaternion_from_axis_angle(normalize(Float3{1, 2, 3}), 1.1F);
    a.voxelScale = {2, .5F, 1.25F};
    document.add_object(std::move(a));
    EditorObject b(2, "B");
    b.polygon = make_editor_polygon_cube(2);
    b.parent = 10;
    b.transform.position = {-.6F, .1F, .9F};
    b.transform.rotation = quaternion_from_axis_angle(normalize(Float3{-1, .5F, .2F}), -2.3F);
    b.voxelScale = {.75F, 1.5F, 3};
    document.add_object(std::move(b));

    const auto m = exact_relative_placement(document, 1, 2);
    check(m.has_value(), "placement computed");
    const auto poseA = document.world_transform(1).value();
    const auto poseB = document.world_transform(2).value();
    for (const Float3 p : {Float3{0, 0, 0}, Float3{1, 0, 0}, Float3{0, 1, 0}, Float3{0, 0, 1}, Float3{.3F, -.7F, 2.1F}}) {
        const auto expected = world_to_voxel(poseA, document.find_object(1)->voxelScale,
                                             voxel_to_world(poseB, document.find_object(2)->voxelScale, p));
        const auto& r = *m;
        const double got[3] = {r[0] * p.x + r[1] * p.y + r[2] * p.z + r[3], r[4] * p.x + r[5] * p.y + r[6] * p.z + r[7],
                               r[8] * p.x + r[9] * p.y + r[10] * p.z + r[11]};
        check(std::abs(got[0] - expected.x) < 2e-4 && std::abs(got[1] - expected.y) < 2e-4 &&
                  std::abs(got[2] - expected.z) < 2e-4,
              "B mesh-local -> A mesh-local placement matches voxel_to_world/world_to_voxel");
    }
    document.find_object(1)->voxelScale = {0, 1, 1};
    check(!exact_relative_placement(document, 1, 2), "zero scale has no placement");
}

void component_round_trip_and_export() {
    TempDir dir;
    EditorDocument document("Exact persistence");
    document.add_object(exact_object(1, "Part", "sources/exact/part-0123456789ab.brep"));
    const auto saved = document.save_transactional(dir.path / "scene.dvescene");
    check(saved.success, "scene saves");
    std::string error;
    auto loaded = EditorDocument::load(saved.manifestPath, &error);
    check(loaded.has_value(), "scene reloads: " + error);
    const auto source = exact_brep_source(*loaded->find_object(1));
    check(source && source->relativePath.generic_string() == "sources/exact/part-0123456789ab.brep" &&
              source->sha256 == "0123456789abcdef" && source->origin == "test",
          "exact source survives save/load");
    const auto exported = export_editor_scene(*loaded, EditorMaterialLibrary::make_default(), dir.path / "runtime.dvoxscene.json");
    check(exported.success, "runtime export: " + exported.error);
    check(read_text(dir.path / "runtime.dvoxscene.json").find("dve.exact_brep") == std::string::npos,
          "the editor-only exact source is not exported to the runtime scene");
}

void fake_import() {
    TempDir dir;
    Fake fake;
    write_text(dir.path / "Bracket.step", "ISO-10303-21;");
    auto result = import_exact_cad(dir.path, dir.path / "Bracket.step", 7, fake_settings(), fake.runner());
    check(bool(result), "fake import: " + result.error);
    check(fake.argv.at(0) == "brepkernel-fake" && fake.argv.at(1) == "import" && fake.has("--mesh") &&
              fake.has("--report") && fake.has("--angular"),
          "import argv");
    check(result.source.relativePath.generic_string() == "sources/exact/Bracket-abcdef012345.brep", "content-addressed name");
    check(read_text(dir.path / result.source.relativePath) == "fake imported brep", "brep adopted into the project");
    check(result.mesh.objectId == 7 && result.mesh.indices.size() == 36, "mesh cooked for the new object");
    check(std::abs(mesh_volume(result.mesh) - 1.0) < 1e-6, "cooked mesh volume");
    check(result.summary.find("step") != std::string::npos, "summary names the format");
    check(fake.pieceCalls == 1, "convex pieces built on import (" + std::to_string(fake.pieceCalls) + " calls)");
    check(std::stod(Fake::option(fake.piecesArgv, "--max-error")) == 1e-4 &&
              Fake::option(fake.piecesArgv, "-o").ends_with("pieces.json"), "convex-pieces argv");
    check(result.collision.component && result.collision.component->type == kConvexSetComponent, "convex set component");
    const auto prisms = convex_set_component_pieces(*result.collision.component);
    check(prisms && prisms->size() == 4, "four pieces decoded");
    check(result.collision.note.find("certified 4 convex pieces") != std::string::npos &&
              result.summary.find("certified 4 convex pieces") != std::string::npos, "the summary reports the pieces");
    fake.piecesRefuse = true;
    auto noPieces = import_exact_cad(dir.path, dir.path / "Bracket.step", 7, fake_settings(), fake.runner());
    check(bool(noPieces) && !noPieces.collision.component && noPieces.collision.refused &&
              noPieces.collision.note.find("collision stays one hull") != std::string::npos,
          "a part that is not an extrusion imports with a note and no pieces");
    fake.piecesRefuse = false;
    auto off = fake_settings();
    off.exactCollision = false;
    const int before = fake.pieceCalls;
    check(bool(import_exact_cad(dir.path, dir.path / "Bracket.step", 7, off, fake.runner())) && fake.pieceCalls == before,
          "convex collision can be switched off");

    check(!import_exact_cad(dir.path, dir.path / "crate.obj", 7, fake_settings(), fake.runner()), "non-CAD refused");
    fake.mode = Fake::Mode::NoLaunch;
    auto missing = import_exact_cad(dir.path, dir.path / "Bracket.step", 7, fake_settings(), fake.runner());
    check(!missing && missing.error.find("DVE_BREPKERNEL") != std::string::npos, "missing tool explains how to install");
    fake.mode = Fake::Mode::Timeout;
    check(import_exact_cad(dir.path, dir.path / "Bracket.step", 7, fake_settings(), fake.runner()).error.find("did not finish") !=
              std::string::npos, "timeout reported");
    fake.mode = Fake::Mode::Error;
    check(import_exact_cad(dir.path, dir.path / "Bracket.step", 7, fake_settings(), fake.runner()).error.find("file not found") !=
              std::string::npos, "tool error passed through");
    fake.mode = Fake::Mode::BadSchema;
    check(import_exact_cad(dir.path, dir.path / "Bracket.step", 7, fake_settings(), fake.runner()).error.find("schema") !=
              std::string::npos, "unknown report schema refused");
}

void fake_boolean() {
    TempDir dir;
    fs::create_directories(dir.path / "sources/exact");
    write_text(dir.path / "sources/exact/a.brep", "a");
    write_text(dir.path / "sources/exact/b.brep", "b");
    EditorDocument document("Exact Boolean");
    auto a = exact_object(1, "Plate", "sources/exact/a.brep");
    a.polygon->materials[0].name = "Steel";
    a.voxelScale = {2, 1, 1};
    document.add_object(std::move(a));
    auto b = exact_object(2, "Peg", "sources/exact/b.brep");
    b.transform.position = {.25F, .5F, 0};
    document.add_object(std::move(b));
    EditorObject plain(3, "Plain");
    plain.polygon = make_editor_polygon_cube(3);
    document.add_object(std::move(plain));
    Fake fake;

    auto result = exact_boolean_editor_polygons(dir.path, document, 1, 2, MeshBoolean::ReverseDifference, fake_settings(),
                                                fake.runner());
    check(bool(result), "fake Boolean: " + result.edit.error);
    check(Fake::option(fake.argv, "--op") == "difference" && fake.has("--reverse") && fake.has("--advisory"),
          "B minus A is difference with --reverse");
    std::vector<double> numbers;
    std::stringstream matrix(Fake::option(fake.argv, "--b-transform"));
    for (std::string item; std::getline(matrix, item, ',');) numbers.push_back(std::stod(item));
    const auto expected = exact_relative_placement(document, 1, 2).value();
    check(numbers.size() == 12, "12 placement numbers");
    for (std::size_t i = 0; i < 12; ++i) check(std::abs(numbers[i] - expected[i]) < 1e-12, "placement passed in full precision");
    check(std::abs(numbers[0] - .5) < 1e-12 && std::abs(numbers[3] - .125) < 1e-12, "B in A's voxelScale frame");
    check(result.edit.mesh.materials.size() == 1 && result.edit.mesh.materials[0].name == "Steel", "A's materials carried over");
    check(result.source.origin == "boolean:reverse_difference", "origin records the operation");
    check(fs::exists(dir.path / result.source.relativePath), "result brep stored in the project");
    check(result.collision.component.has_value(), "the exact Boolean result gets convex pieces");
    check(result.summary.find("Certified exact Boolean: volume 2 m3 (exact, world scale)") == 0, "certified summary in world scale: " + result.summary);

    fake.mode = Fake::Mode::Refuse;
    const auto filesBefore = std::distance(fs::directory_iterator(dir.path / "sources/exact"), fs::directory_iterator{});
    auto refused = exact_boolean_editor_polygons(dir.path, document, 1, 2, MeshBoolean::Intersection, fake_settings(), fake.runner());
    check(!refused && refused.refused && refused.refusalKind == "IntersectionCompletenessProbeFailed", "refusal recognized");
    check(refused.edit.error.find("a section branch is unaccounted for") != std::string::npos, "refusal reason shown");
    check(refused.advisory.find("occt 3.02661") != std::string::npos && refused.advisory.find("smlib 3.01443") != std::string::npos &&
              refused.advisory.find("contested") != std::string::npos,
          "advisory second opinions summarized: " + refused.advisory);
    check(std::distance(fs::directory_iterator(dir.path / "sources/exact"), fs::directory_iterator{}) == filesBefore,
          "a refusal stores nothing");

    fake.mode = Fake::Mode::Empty;
    auto empty = exact_boolean_editor_polygons(dir.path, document, 1, 2, MeshBoolean::Intersection, fake_settings(), fake.runner());
    check(!empty && empty.empty && !empty.refused, "certified empty result is reported, not applied");

    fake.calls = 0;
    auto noSource = exact_boolean_editor_polygons(dir.path, document, 1, 3, MeshBoolean::Union, fake_settings(), fake.runner());
    check(!noSource && noSource.edit.error.find("no exact CAD source") != std::string::npos && fake.calls == 0,
          "operand without exact source refused before running the tool");
    fs::remove(dir.path / "sources/exact/b.brep");
    auto gone = prepare_exact_boolean(dir.path, document, 1, 2, MeshBoolean::Union);
    check(gone.error.find("missing") != std::string::npos, "missing exact file reported");

    const auto stamp = exact_operand_stamp(document, 1, 2);
    document.find_object(2)->transform.position.x += .01F;
    check(exact_operand_stamp(document, 1, 2) != stamp, "stamp changes when an operand moves");
}

// --- real brepkernel ----------------------------------------------------------------------

bool run_cli(const ExactCadSettings& settings, std::vector<std::string> args, std::string* output = nullptr) {
    auto argv = settings.command;
    argv.insert(argv.end(), args.begin(), args.end());
    const auto result = run_exact_cad_process(argv, std::chrono::seconds(300), 1U << 20U);
    if (output) *output = result.output + result.error;
    return result.launched && !result.timedOut && result.exitCode == 0;
}

bool real_tool_available(const ExactCadSettings& settings, const fs::path& dir) {
    std::string output;
    const bool ok = run_cli(settings, {"primitive", "box", "-o", (dir / "probe.brep").string(), "--report",
                                       (dir / "probe.json").string()}, &output);
    if (!ok) {
        std::cout << "SKIP real brepkernel tests: could not run";
        for (const auto& a : settings.command) std::cout << ' ' << a;
        std::cout << " (" << output.substr(0, 200) << ")\n";
    }
    return ok;
}

void real_brepkernel(const ExactCadSettings& settings, const fs::path& root) {
    const auto make = [&](std::vector<std::string> args) {
        args.push_back("--report");
        args.push_back((root / "make.json").string());
        std::string output;
        check(run_cli(settings, args, &output), "primitive: " + output);
    };
    make({"primitive", "box", "-o", (root / "plate.brep").string(), "--size", "1", "1", "1"});
    make({"primitive", "cylinder", "-o", (root / "peg.brep").string(), "--radius", "0.2", "--height", "3"});
    make({"primitive", "cylinder", "-o", (root / "rod.brep").string(), "--radius", "1", "--height", "4", "--axis", "y"});

    auto plate = import_exact_cad(root, root / "plate.brep", 1, settings);
    check(bool(plate), "real import: " + plate.error);
    check(std::abs(mesh_volume(plate.mesh) - 1.0) < 1e-6, "box mesh volume");
    auto peg = import_exact_cad(root, root / "peg.brep", 2, settings);
    check(bool(peg), "real import: " + peg.error);

    EditorDocument document("Real exact");
    EditorObject a(1, "Plate");
    a.polygon = plate.mesh;
    a.voxelScale = {2, 1, 1};                       // plate is 2 x 1 x 1 in the world
    Component ca = make_exact_brep_component(plate.source);
    ca.id = 1;
    a.components.push_back(ca);
    document.add_object(std::move(a));
    EditorObject b(2, "Peg");
    b.polygon = peg.mesh;
    b.transform.rotation = quaternion_from_axis_angle({0, 0, 1}, 1.5707963267948966F);   // axis along world x
    Component cb = make_exact_brep_component(peg.source);
    cb.id = 1;
    b.components.push_back(cb);
    document.add_object(std::move(b));

    // The peg (length 3) runs right through the 2 m wide plate: world volume 2 - pi r^2 * 2.
    // A's mesh frame is the world squeezed by voxelScale x = 2, so the mesh volume is half.
    auto result = exact_boolean_editor_polygons(root, document, 1, 2, MeshBoolean::Difference, settings);
    check(bool(result), "real exact difference: " + result.edit.error);
    const double expected = (2.0 - 3.141592653589793 * .2 * .2 * 2.0) / 2.0;
    const double got = mesh_volume(result.edit.mesh);
    check(std::abs(got - expected) < 5e-3 * expected,
          "difference mesh volume " + std::to_string(got) + " vs exact " + std::to_string(expected));
    check(diagnose_polygon_topology(result.edit.mesh).boundaries == 0, "exact Boolean mesh is closed");
    check(fs::exists(root / result.source.relativePath), "certified .brep stored");
    std::cout << "real exact difference: " << result.summary << "; mesh volume in A's frame " << got
              << " vs exact " << expected << "\n";

    // Two crossing cylinders of equal radius: brepkernel refuses the intersection.
    auto rod = import_exact_cad(root, root / "rod.brep", 3, settings);
    check(bool(rod), "rod import: " + rod.error);
    EditorDocument cross("Cross");
    for (EditorObjectId id : {EditorObjectId(1), EditorObjectId(2)}) {
        EditorObject o(id, id == 1 ? "RodY" : "RodX");
        o.polygon = rod.mesh;
        o.polygon->objectId = id;
        if (id == 2) o.transform.rotation = quaternion_from_axis_angle({0, 0, 1}, 1.5707963267948966F);
        Component c = make_exact_brep_component(rod.source);
        c.id = 1;
        o.components.push_back(c);
        cross.add_object(std::move(o));
    }
    auto refused = exact_boolean_editor_polygons(root, cross, 1, 2, MeshBoolean::Intersection, settings);
    if (refused.refused) {
        std::cout << "real refusal: " << refused.edit.error << "\n  " << refused.advisory << "\n";
        check(!refused.refusalKind.empty(), "refusal kind reported");
    } else {
        std::cout << "note: crossing cylinders were certified here ("
                  << (refused ? refused.summary : refused.edit.error) << ")\n";
    }
}

void native_commands(const fs::path& root) {
    fs::create_directories(root / "assets");
    fs::copy_file(root / "plate.brep", root / "assets/plate.brep", fs::copy_options::overwrite_existing);
    fs::copy_file(root / "peg.brep", root / "assets/peg.brep", fs::copy_options::overwrite_existing);
    fs::copy_file(root / "rod.brep", root / "assets/rod.brep", fs::copy_options::overwrite_existing);
    NativeEditorController controller;
    controller.configure_ai_assistant(root);
    (void)controller.dispatch_action("window.toggle_assets");
    const auto import = [&](const char* path) {
        const auto* asset = controller.asset_database().find_path(path);
        check(asset != nullptr, std::string("asset indexed: ") + path);
        controller.asset_browser_state().selectedId = asset->id;
        controller.refresh_menu_state();
        const auto* action = controller.workspace().menus().find("polygon.import_exact");
        check(action && action->enabled, "Import Exact CAD enabled for a .brep asset: " + (action ? action->disabledReason : std::string("no action")));
        check(controller.dispatch_action("polygon.import_exact"), "Import Exact CAD starts");
        check(controller.wait_for_long_job(300.0) || controller.status().text.find("Imported exact") != std::string::npos,
              "import job finishes");
        check(controller.status().text.find("Imported exact") != std::string::npos, "import status: " + controller.status().text);
        return *controller.workspace().selected_object();
    };
    const auto a = import("assets/plate.brep");
    const auto b = import("assets/peg.brep");
    auto& document = controller.workspace().document();
    check(exact_brep_source(*document.find_object(a)).has_value(), "imported object carries its exact source");
    document.find_object(b)->transform.position = document.find_object(a)->transform.position;
    controller.workspace().select_object(b);
    controller.workspace().add_to_selection(a);   // A is the primary (active) object
    controller.refresh_menu_state();
    const auto* action = controller.workspace().menus().find("mesh.exact_boolean_difference");
    check(action && action->enabled, "exact Boolean enabled for two exact operands: " + (action ? action->disabledReason : ""));
    const auto objectsBefore = document.objects().size();
    check(controller.dispatch_action("mesh.exact_boolean_difference"), "exact Boolean starts");
    (void)controller.wait_for_long_job(300.0);
    check(document.objects().size() == objectsBefore + 1, "one result object added: " + controller.status().text);
    const auto result = *controller.workspace().selected_object();
    check(document.find_object(result)->name.find("(Exact Boolean)") != std::string::npos, "result named");
    check(exact_brep_source(*document.find_object(result)).has_value(), "result carries its exact source");
    const auto pieces = active_convex_set(document.find_object(result)->components);
    check(pieces.size() > 4, "the plate-with-hole result carries certified convex pieces (" + std::to_string(pieces.size()) + ")");
    // rebuild them with the command (one undo step, replaces the set)
    controller.workspace().select_object(result);
    controller.refresh_menu_state();
    const auto* rebuild = controller.workspace().menus().find("mesh.exact_collision");
    check(rebuild && rebuild->enabled, "Build Exact Convex Collision is enabled for an exact object");
    check(controller.dispatch_action("mesh.exact_collision"), "rebuild starts");
    (void)controller.wait_for_long_job(300.0);
    std::size_t sets = 0;
    for (const auto& c : document.find_object(result)->components) sets += c.type == kConvexSetComponent;
    check(sets == 1, "rebuilding replaces the set: " + controller.status().text);
    check(controller.dispatch_action("edit.undo"), "undo the rebuild");
    check(controller.dispatch_action("edit.undo") && document.objects().size() == objectsBefore, "one undo step removes it");

    // A refusal changes nothing; with Manifold built in, its uncertified preview opens instead.
    const auto rodY = import("assets/rod.brep");
    const auto rodX = import("assets/rod.brep");
    document.find_object(rodX)->transform = document.find_object(rodY)->transform;
    document.find_object(rodX)->transform.rotation = quaternion_from_axis_angle({0, 0, 1}, 1.5707963267948966F);
    controller.workspace().select_object(rodX);
    controller.workspace().add_to_selection(rodY);
    controller.refresh_menu_state();
    const auto count = document.objects().size();
    check(controller.dispatch_action("mesh.exact_boolean_intersection"), "exact intersection starts");
    (void)controller.wait_for_long_job(300.0);
    const auto& status = controller.status().text;
    std::cout << "native intersection: " << status << "\n";
    if (status.find("refused") != std::string::npos) {
        check(document.objects().size() == count, "a refusal adds nothing");
        check(status.find("Uncertified second opinions") != std::string::npos, "advisory shown with the refusal");
        if (mesh_boolean_available())
            check(status.find("Manifold mesh preview") != std::string::npos && controller.mesh_authoring_hidden_objects().size() == 2,
                  "Manifold fallback preview opened");
    } else {
        check(document.objects().size() == count + 1, "certified intersection added");
    }
}

} // namespace

// A scene can name any path in dve.exact_brep; only project-contained relative sources run.
void source_outside_project_refused() {
    TempDir dir;
    fs::create_directories(dir.path / "sources/exact");
    write_text(dir.path / "sources/exact/a.brep", "a");
    write_text(dir.path / "outside.brep", "o");
    for (const std::string bad : {std::string("../outside.brep"), std::string("sources/../../outside.brep"),
                                  (dir.path / "outside.brep").generic_string()}) {
        EditorDocument document("Outside");
        document.add_object(exact_object(1, "A", "sources/exact/a.brep"));
        document.add_object(exact_object(2, "B", bad));
        const auto request = prepare_exact_boolean(dir.path, document, 1, 2, MeshBoolean::Union);
        check(request.error.find("outside the project") != std::string::npos, "source outside project refused: " + bad);
    }
}

int main() {
    try {
        placement_matches_voxel_metric();
        component_round_trip_and_export();
        fake_import();
        fake_boolean();
        source_outside_project_refused();
        TempDir dir;
        const auto settings = ExactCadSettings::from_environment();
        const char* require = std::getenv("DVE_REQUIRE_BREPKERNEL");
        if (real_tool_available(settings, dir.path)) {
            real_brepkernel(settings, dir.path);
            native_commands(dir.path);
        } else if (require && std::strcmp(require, "1") == 0) {
            throw std::runtime_error("DVE_REQUIRE_BREPKERNEL=1 but brepkernel could not run");
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "editor exact CAD tests passed\n";
    return 0;
}
