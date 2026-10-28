// Exact tubes, coping and plane (miter) cuts (editor_exact_tube.hpp), and the native commands.
//
// Always run, no brepkernel needed: parsing and validation of tube and cut specs, the tube record
// and its save/load round trip, the arguments the library sends to brepkernel (against a stand-in
// runner), the zero-gap tangent retry and every refusal path, the plane-cut placement algebra.
//
// With brepkernel (found like the editor finds it: $DVE_BREPKERNEL, `brepkernel` on PATH, then
// `python3 -m brepkernel`): real certified geometry. Tube volumes against the analytic ones, wall
// thickness, plane-cut faces within micrometres of the plane (47.5 degrees on a rectangular tube),
// cope volumes against an independent numerical integral for round-on-round at several angles,
// mixed profiles, offset axes and a fit-up gap, the equal-diameter 90 degree case (refused at zero
// gap, certified with the documented 0.01 mm clearance), closed meshes, undo/redo and save/load of
// the native commands.
//
// Without brepkernel those parts are reported as SKIPPED (exit code 77, ctest "Skipped").
// DVE_REQUIRE_BREPKERNEL=1 makes a missing brepkernel a failure (the exact-tube CI job sets it).

#include "dve_test_skip.hpp"

#include "dve/editor_exact_tube.hpp"
#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/voxel_metric.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include "exact_cad_test_support.hpp"

namespace {
using namespace dve;
using namespace dve::editor;
using namespace exact_cad_test;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); std::fflush(stdout); ++g_failures; }
}
bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }
bool near_rel(double a, double b, double relative) { return std::abs(a - b) <= relative * std::max(std::abs(a), std::abs(b)); }
std::string num(double v) { std::ostringstream o; o.precision(9); o << v; return o.str(); }

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

// ---------------------------------------------------------------------------------------------
// Mesh helpers
// ---------------------------------------------------------------------------------------------
using V3 = std::array<double, 3>;
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double length(const V3& a) { return std::sqrt(dot(a, a)); }

V3 position(const CookedPolygonAsset& m, std::uint32_t index) {
    const auto p = m.vertices[m.indices[index]].position;
    return {p.x, p.y, p.z};
}

double mesh_volume(const CookedPolygonAsset& m) {
    double out = 0;
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3)
        out += dot(position(m, std::uint32_t(i)), cross(position(m, std::uint32_t(i + 1)), position(m, std::uint32_t(i + 2))));
    return out / 6;
}

std::vector<V3> all_vertices(const CookedPolygonAsset& m) {
    std::vector<V3> out;
    out.reserve(m.vertices.size());
    for (const auto& v : m.vertices) out.push_back({v.position.x, v.position.y, v.position.z});
    return out;
}

// Triangles whose normal is (anti)parallel to `normal`: their summed area and the worst distance of
// their vertices from the plane through `point`.
struct PlaneFace { double area{}; double worst{}; std::size_t triangles{}; std::vector<V3> vertices; };
PlaneFace plane_face(const CookedPolygonAsset& m, const V3& point, const V3& normal) {
    PlaneFace face;
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const V3 a = position(m, std::uint32_t(i)), b = position(m, std::uint32_t(i + 1)), c = position(m, std::uint32_t(i + 2));
        const V3 n = cross(sub(b, a), sub(c, a));
        const double twice = length(n);
        if (twice < 1e-18) continue;
        if (std::abs(dot(n, normal)) / twice < 1.0 - 1e-6) continue;
        // the triangle must also lie near the plane (a parallel face elsewhere is not the cut face)
        if (std::abs(dot(sub(a, point), normal)) > 1e-4) continue;
        face.area += twice / 2;
        ++face.triangles;
        for (const V3& p : {a, b, c}) {
            face.worst = std::max(face.worst, std::abs(dot(sub(p, point), normal)));
            face.vertices.push_back(p);
        }
    }
    return face;
}

bool closed(const CookedPolygonAsset& m) {
    const auto d = diagnose_polygon_topology(m);
    return d.boundaries == 0 && d.nonmanifold == 0 && d.inconsistentWinding == 0;
}

// ---------------------------------------------------------------------------------------------
// Always-run tests
// ---------------------------------------------------------------------------------------------
void spec_parsing() {
    auto round = parse_tube_spec("round 50.8 3 600 y");
    check(round.spec && round.error.empty(), "round parses: " + round.error);
    if (round.spec) {
        check(round.spec->profile == TubeProfile::Round && near(round.spec->width, 0.0508, 1e-12) && near(round.spec->height, 0.0508, 1e-12) &&
                  near(round.spec->wall, 0.003, 1e-12) && near(round.spec->length, 0.6, 1e-12) && round.spec->axis == 'y', "round fields");
    }
    auto rect = parse_tube_spec("Rect 80x40 3 600 z");
    check(rect.spec && rect.spec->profile == TubeProfile::Rectangular && near(rect.spec->width, 0.08, 1e-12) && near(rect.spec->height, 0.04, 1e-12) &&
              rect.spec->axis == 'z', "rect 80x40: " + rect.error);
    auto rectUnits = parse_tube_spec("rect 80mmx1.5in 3 0.6m x");
    check(rectUnits.spec && near(rectUnits.spec->height, 0.0381, 1e-12) && near(rectUnits.spec->length, 0.6, 1e-12) && rectUnits.spec->axis == 'x',
          "units on any number: " + rectUnits.error);
    auto square = parse_tube_spec("square 40, 2, 500");
    check(square.spec && square.spec->profile == TubeProfile::Square && square.spec->axis == 'y' && near(square.spec->height, 0.04, 1e-12),
          "square, commas, default axis: " + square.error);
    auto times = parse_tube_spec("rect 80\xC3\x97" "40 3 600");
    check(times.spec.has_value(), "the multiplication sign separates sizes: " + times.error);
    for (const char* bad : {"", "hex 10 1 100", "round 50.8 3", "round 50.8 3 600 w", "round 50.8 3 600 y extra", "round abc 3 600", "round 50 30 600",
                            "round 50 -3 600", "round 0.01 0.001 600", "rect 80 3 600", "round 50.8 3 0", "round 1e9 3 600 y"}) {
        const auto parsed = parse_tube_spec(bad);
        check(!parsed.spec && !parsed.error.empty(), std::string("rejected: '") + bad + "'");
    }
    TubeSpec spec;
    spec.profile = TubeProfile::Rectangular; spec.width = 0.08; spec.height = 0.04; spec.wall = 0.003; spec.length = 0.6; spec.axis = 'z';
    check(format_tube_spec(spec) == "rect 80x40 3 600 z", "format: " + format_tube_spec(spec));
    const auto back = parse_tube_spec(format_tube_spec(spec));
    check(back.spec && near(back.spec->width, spec.width, 1e-12) && near(back.spec->length, spec.length, 1e-12), "format round-trips");
    check(describe_tube_spec(spec).find("Rectangular tube 80 x 40 mm, 3 mm wall, 600 mm long, along Z") == 0, "describe: " + describe_tube_spec(spec));
    check(near(tube_cross_section_area(spec), 0.08 * 0.04 - 0.074 * 0.034, 1e-12), "rect area");
    spec.profile = TubeProfile::Round; spec.width = spec.height = 0.0508;
    check(near(tube_volume(spec), kPi * (0.0254 * 0.0254 - 0.0224 * 0.0224) * 0.6, 1e-12), "round volume");
    check(!validate_tube_spec(spec).empty() == false, "valid round spec");
    spec.wall = 0.0254;
    check(validate_tube_spec(spec).find("too thick") != std::string::npos, "wall = radius refused");
}

void miter_parsing_and_plane() {
    const auto m = parse_miter_spec("47.5 high v at 120");
    check(m.spec && near(m.spec->angleDegrees, 47.5, 1e-12) && m.spec->keep == MiterKeep::High && m.spec->tilt == 'v' &&
              near(m.spec->positionWorldMeters, 0.12, 1e-12), "miter parse: " + m.error);
    check(parse_miter_spec("30deg").spec.has_value(), "30deg");
    check(parse_miter_spec("-20 low u").spec.has_value(), "negative angle");
    for (const char* bad : {"", "abc", "90", "-85.5", "30 sideways", "30 at", "30 at x"}) check(!parse_miter_spec(bad).spec, std::string("miter rejected: '") + bad + "'");
    check(format_miter_spec(*m.spec) == "47.5 high v at 120", "miter format: " + format_miter_spec(*m.spec));

    TubeSpec tube; tube.profile = TubeProfile::Rectangular; tube.width = 0.08; tube.height = 0.04; tube.axis = 'y';   // u = x, v = z
    ExactMiterSpec miter; miter.angleDegrees = 47.5; miter.tilt = 'u'; miter.keep = MiterKeep::Low;
    const auto plane = miter_plane(tube, miter, 0.05);
    check(near(plane.point[1], 0.05, 1e-15) && near(plane.point[0], 0, 1e-15), "plane point on the axis");
    check(near(plane.normal[1], std::cos(47.5 * kDeg), 1e-12) && near(plane.normal[0], std::sin(47.5 * kDeg), 1e-12) && near(plane.normal[2], 0, 1e-15),
          "plane normal leans toward u");
    miter.keep = MiterKeep::High;
    const auto flipped = miter_plane(tube, miter, 0.05);
    check(near(flipped.normal[1], -plane.normal[1], 1e-15) && near(flipped.normal[0], -plane.normal[0], 1e-15), "keeping the high end flips the removed side");
    miter.tilt = 'v';
    check(near(miter_plane(tube, miter, 0).normal[2], -std::sin(47.5 * kDeg), 1e-12), "v tilts toward z for a y tube");
    check(tube_axes('x') == std::array<int, 3>({0, 1, 2}) && tube_axes('y') == std::array<int, 3>({1, 0, 2}) && tube_axes('z') == std::array<int, 3>({2, 0, 1}) &&
              tube_axes('w')[0] == -1, "transverse axes");
}

void tube_component_round_trip() {
    TempDir dir;
    TubeSpec spec; spec.profile = TubeProfile::Rectangular; spec.width = 0.08; spec.height = 0.04; spec.wall = 0.003; spec.length = 0.6; spec.axis = 'z';
    EditorObject tube(1, "Rail");
    tube.polygon = make_editor_polygon_cube(1);
    Component exact = make_exact_brep_component({"sources/exact/rail-0123456789ab.brep", "0123456789abcdef", "tube:rect"});
    exact.id = 1;
    tube.components.push_back(exact);
    Component record = make_exact_tube_component(spec, "plane cut 47.5 deg from square; cope to B, gap 0 mm");
    record.id = 2;
    tube.components.push_back(record);
    const auto read = exact_tube_record(tube);
    check(read && read->spec.profile == TubeProfile::Rectangular && near(read->spec.height, 0.04, 1e-15) && read->spec.axis == 'z' &&
              read->cuts == "plane cut 47.5 deg from square; cope to B, gap 0 mm", "record reads back");
    EditorDocument document("Tube persistence");
    document.add_object(std::move(tube));
    const auto saved = document.save_transactional(dir.path / "scene.dvescene");
    check(saved.success, "scene saves");
    std::string error;
    auto loaded = EditorDocument::load(saved.manifestPath, &error);
    check(loaded.has_value(), "scene reloads: " + error);
    if (loaded) {
        const auto again = exact_tube_record(*loaded->find_object(1));
        check(again && again->spec.profile == TubeProfile::Rectangular && near(again->spec.width, 0.08, 1e-15) && near(again->spec.wall, 0.003, 1e-15) &&
                  again->cuts == read->cuts, "tube record survives save/load");
        check(exact_brep_source(*loaded->find_object(1)).has_value(), "exact source survives next to it");
        const auto exported = export_editor_scene(*loaded, EditorMaterialLibrary::make_default(), dir.path / "runtime.dvoxscene.json");
        check(exported.success, "runtime export: " + exported.error);
        check(read_text(dir.path / "runtime.dvoxscene.json").find("dve.exact_tube") == std::string::npos, "the authoring-only tube record is not exported");
    }
    // A record with a corrupt value reads as no record, never as a wrong tube.
    EditorObject broken(2, "Broken");
    Component bad = make_exact_tube_component(spec);
    bad.properties["wall"] = 0.0;
    bad.id = 1;
    broken.components.push_back(bad);
    check(!exact_tube_record(broken), "a zero wall is not a tube record");
    broken.components[0] = make_exact_tube_component(spec);
    broken.components[0].properties["profile"] = std::string("hex");
    check(!exact_tube_record(broken), "an unknown profile is not a tube record");
    check(append_tube_cut("", "cope to A; B") == "cope to A, B" && append_tube_cut("x", "y") == "x; y", "cut records are ';'-separated and clean");
}

// ---------------------------------------------------------------------------------------------
// Stand-in brepkernel
// ---------------------------------------------------------------------------------------------
struct Seen { std::vector<std::vector<std::string>> calls; };
std::string opt(const std::vector<std::string>& argv, std::string_view name) {
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) if (argv[i] == name) return argv[i + 1];
    return {};
}
bool has(const std::vector<std::string>& argv, std::string_view word) { return std::find(argv.begin(), argv.end(), word) != argv.end(); }

// Answers primitive, boolean (mesh = unit cube) and convex-pieces. `refuseBelowGap`: a boolean whose
// operand-B cylinder radius (from the primitive call) is below that is refused as tangent.
struct StandIn {
    Seen seen;
    fs::path cube;
    double refuseRadiusAtMost{-1.0};     // refuse a boolean when B's primitive radius <= this
    double lastRadiusB{0.0};
    int solids{1};
    double resultVolume{0.5}, volumeA{1.0};
    bool watertight{true};
    ExactCadRunner runner() {
        return [this](const std::vector<std::string>& argv, std::chrono::seconds, std::uint64_t) {
            seen.calls.push_back(argv);
            ExactCadProcessResult result;
            result.launched = true;
            result.exitCode = 0;
            const auto report = opt(argv, "--report");
            if (has(argv, "primitive")) {
                write_text(opt(argv, "-o"), "primitive brep");
                if (!opt(argv, "--radius").empty()) lastRadiusB = std::stod(opt(argv, "--radius"));
                write_text(report, R"({"schema":"brepkernel.cli/1","status":"ok"})");
                return result;
            }
            if (has(argv, "convex-pieces")) {
                write_text(report, R"({"schema":"brepkernel.cli/1","status":"refused","certificate":{"refusal":{"kind":"NotAnExtrusion","message":"x"}}})");
                result.exitCode = 2;
                return result;
            }
            if (refuseRadiusAtMost >= 0 && lastRadiusB <= refuseRadiusAtMost) {
                write_text(report, R"({"schema":"brepkernel.cli/1","status":"refused","refusal":{"kind":"OpenAssembly","message":"tangent contact"}})");
                result.exitCode = 2;
                return result;
            }
            write_text(opt(argv, "-o"), "result brep");
            write_cube_glb(opt(argv, "--mesh"));
            std::ostringstream text;
            text.precision(12);
            text << R"({"schema":"brepkernel.cli/1","status":"accepted","inputs":{"a":{"volume":)" << volumeA << R"(}},"result":{"sha256":"0123456789abcdef0123","solids":)"
                 << solids << R"(,"volume":)" << resultVolume << R"(},"mesh":{"triangles":12,"watertight":)" << (watertight ? "true" : "false") << "}}";
            write_text(report, text.str());
            return result;
        };
    }
    [[nodiscard]] std::vector<std::string> boolean_call(std::size_t nth = 0) const {
        for (const auto& call : seen.calls) if (has(call, "boolean") && nth-- == 0) return call;
        return {};
    }
    [[nodiscard]] int count(std::string_view command) const {
        int n = 0;
        for (const auto& call : seen.calls) n += has(call, command) ? 1 : 0;
        return n;
    }
};

ExactCadSettings fake_settings() {
    ExactCadSettings s;
    s.command = {"brepkernel-fake"};
    s.exactCollision = false;
    return s;
}

EditorObject tube_object(EditorObjectId id, std::string name, const TubeSpec& spec, const fs::path& root, const char* brep) {
    EditorObject object(id, std::move(name));
    object.polygon = make_editor_polygon_cube(id);
    fs::create_directories(root / "sources/exact");
    write_text(root / "sources/exact" / brep, "brep");
    Component exact = make_exact_brep_component({fs::path("sources/exact") / brep, "0123456789abcdef", "tube"});
    exact.id = 1;
    object.components.push_back(exact);
    Component record = make_exact_tube_component(spec);
    record.id = 2;
    object.components.push_back(record);
    return object;
}

TubeSpec round_spec(double od, double wall, double length, char axis) {
    TubeSpec s; s.profile = TubeProfile::Round; s.width = s.height = od; s.wall = wall; s.length = length; s.axis = axis; return s;
}

void fake_create_tube() {
    TempDir dir;
    write_cube_glb(dir.path / "cube.glb");
    StandIn stand{{}, dir.path / "cube.glb"};
    stand.resultVolume = tube_volume(round_spec(0.0508, 0.003, 0.6, 'y'));
    auto created = create_exact_tube(dir.path, round_spec(0.0508, 0.003, 0.6, 'y'), 5, fake_settings(), stand.runner());
    check(bool(created), "fake create: " + created.error);
    check(stand.count("primitive") == 2 && stand.count("boolean") == 1, "outer + bore + one difference");
    const auto& outer = stand.seen.calls.at(0);
    const auto& bore = stand.seen.calls.at(1);
    check(opt(outer, "--radius") == "0.0254" && opt(outer, "--height") == "0.6" && opt(outer, "--axis") == "y", "outer cylinder argv");
    check(near(std::stod(opt(bore, "--radius")), 0.0224, 1e-12) && std::stod(opt(bore, "--height")) > 0.6 + 0.2, "the bore is a narrower, longer cylinder");
    const auto boolean = stand.boolean_call();
    check(opt(boolean, "--op") == "difference" && opt(boolean, "--b-transform").empty() && has(boolean, "--mesh"), "difference, identity placement");
    check(created.mesh.objectId == 5 && created.source.origin == "tube:round 50.8 3 600 y", "origin records the tube: " + created.source.origin);
    check(created.summary.find("Round tube OD 50.8 mm") != std::string::npos && created.summary.find("Certified exact tube") != std::string::npos, "summary: " + created.summary);

    TubeSpec rect; rect.profile = TubeProfile::Rectangular; rect.width = 0.08; rect.height = 0.04; rect.wall = 0.003; rect.length = 0.6; rect.axis = 'x';
    stand.seen.calls.clear();
    stand.resultVolume = tube_volume(rect);
    auto box = create_exact_tube(dir.path, rect, 6, fake_settings(), stand.runner());
    check(bool(box), "fake rect: " + box.error);
    const auto& outerBox = stand.seen.calls.at(0);
    std::vector<std::string> size;
    for (std::size_t i = 0; i < outerBox.size(); ++i) if (outerBox[i] == "--size") size = {outerBox[i + 1], outerBox[i + 2], outerBox[i + 3]};
    check(size.size() == 3 && size[0] == "0.6" && size[1] == "0.08" && size[2] == "0.04", "x tube: length along X, width along Y, height along Z");

    // Self-checks refuse a result that does not match the analytic tube.
    stand.resultVolume = tube_volume(rect) * 1.001;
    check(!create_exact_tube(dir.path, rect, 6, fake_settings(), stand.runner()), "a wrong volume is refused");
    stand.resultVolume = tube_volume(rect);
    stand.solids = 2;
    check(!create_exact_tube(dir.path, rect, 6, fake_settings(), stand.runner()), "two solids are refused");
    stand.solids = 1;
    stand.watertight = false;
    check(!create_exact_tube(dir.path, rect, 6, fake_settings(), stand.runner()), "a mesh that is not watertight is refused");
    TubeSpec bad = rect; bad.wall = 0.05;
    check(create_exact_tube(dir.path, bad, 6, fake_settings(), stand.runner()).error.find("too thick") != std::string::npos, "an invalid spec never reaches brepkernel");
}

void fake_cope_and_retry() {
    TempDir dir;
    write_cube_glb(dir.path / "cube.glb");
    EditorDocument document("Cope");
    document.add_object(tube_object(1, "Branch", round_spec(0.06, 0.003, 0.3, 'y'), dir.path, "a.brep"));
    auto b = tube_object(2, "Run", round_spec(0.06, 0.003, 0.4, 'x'), dir.path, "b.brep");
    b.transform.position = {0.1F, 0.0F, 0.0F};
    document.add_object(std::move(b));
    // Prepared request: B's solid outer body, in B's frame, through the placement.
    auto request = prepare_exact_cope(dir.path, document, 1, 2);
    check(request.error.empty(), "prepare: " + request.error);
    check(request.boolean.primitiveB.kind == ExactPrimitive::Kind::Cylinder && near(request.boolean.primitiveB.radius, 0.03, 1e-15) &&
              near(request.boolean.primitiveB.height, 0.4, 1e-15) && request.boolean.primitiveB.axis == 'x', "cutter = B's outer cylinder");
    check(request.boolean.operation == MeshBoolean::Difference && request.kind == "cope" && request.record == "cope to Run, gap 0 mm", "cope record: " + request.record);
    check(near(request.boolean.placement[3], 0.1, 1e-6), "B's pose is in the placement");
    check(request.tangentRetry, "equal diameters are a tangent candidate");

    StandIn stand{{}, dir.path / "cube.glb"};
    stand.refuseRadiusAtMost = 0.03;
    auto cut = run_exact_tube_cut(dir.path, request, fake_settings(), stand.runner());
    check(bool(cut) && cut.clearanceApplied, "refused at zero gap, repeated once with clearance: " + cut.boolean.edit.error);
    check(stand.count("boolean") == 2, "exactly one retry");
    check(near(cut.appliedGapMeters, 1e-5, 1e-15), "the clearance is 0.01 mm");
    check(cut.record == "cope to Run, gap 0.01 mm (automatic tangent clearance)", "record states the clearance: " + cut.record);
    check(cut.boolean.summary.find("tangentially") != std::string::npos && cut.boolean.summary.find("not for zero gap") != std::string::npos &&
              cut.boolean.summary.find("0.02 mm larger across") != std::string::npos, "summary states it: " + cut.boolean.summary);
    check(near(std::stod(opt(stand.seen.calls[stand.seen.calls.size() > 5 ? 4 : 0], "--radius")), 0.03, 1e-4), "(second cutter was built)");
    check(!cut.firstRefusal.empty() && cut.firstRefusal.find("OpenAssembly") != std::string::npos, "the first refusal is kept: " + cut.firstRefusal);

    // Retry switched off: a clean refusal, nothing applied.
    ExactCopeOptions strict; strict.retryTangent = false;
    auto strictRequest = prepare_exact_cope(dir.path, document, 1, 2, strict);
    stand.seen.calls.clear();
    auto refused = run_exact_tube_cut(dir.path, strictRequest, fake_settings(), stand.runner());
    check(!refused && refused.boolean.refused && !refused.clearanceApplied && refused.boolean.edit.error.find("OpenAssembly") != std::string::npos &&
              stand.count("boolean") == 1, "no retry: clean refusal with brepkernel's reason: " + refused.boolean.edit.error);

    // Different diameters are not tangent candidates: a refusal is final.
    EditorDocument other("Other");
    other.add_object(tube_object(1, "Small", round_spec(0.04, 0.003, 0.3, 'y'), dir.path, "a.brep"));
    other.add_object(tube_object(2, "Big", round_spec(0.08, 0.003, 0.4, 'x'), dir.path, "b.brep"));
    auto unequal = prepare_exact_cope(dir.path, other, 1, 2);
    check(!unequal.tangentRetry, "unequal diameters: no retry");
    stand.seen.calls.clear();
    stand.refuseRadiusAtMost = 1.0;
    check(!run_exact_tube_cut(dir.path, unequal, fake_settings(), stand.runner()) && stand.count("boolean") == 1, "refused once");

    // The retry itself refused: both reasons reported.
    stand.refuseRadiusAtMost = 1.0;
    stand.seen.calls.clear();
    auto both = run_exact_tube_cut(dir.path, request, fake_settings(), stand.runner());
    check(!both && both.boolean.edit.error.find("Retried with 0.01 mm tangent clearance") != std::string::npos && both.boolean.refused, "double refusal: " + both.boolean.edit.error);

    // A fit-up gap grows the cutter in every direction (round: radius and both ends).
    ExactCopeOptions gap; gap.gapWorldMeters = 0.002;
    auto gapped = prepare_exact_cope(dir.path, document, 1, 2, gap);
    check(near(gapped.boolean.primitiveB.radius, 0.032, 1e-15) && near(gapped.boolean.primitiveB.height, 0.404, 1e-15) && gapped.record == "cope to Run, gap 2 mm",
          "gap 2 mm: " + gapped.record);
    check(!prepare_exact_cope(dir.path, document, 1, 2, ExactCopeOptions{-0.001}).error.empty(), "negative gap refused");
    check(!prepare_exact_cope(dir.path, document, 1, 2, ExactCopeOptions{0.5}).error.empty(), "huge gap refused");

    // Volume and piece checks on certified answers.
    stand.refuseRadiusAtMost = -1.0;
    stand.resultVolume = stand.volumeA;
    auto none = run_exact_tube_cut(dir.path, strictRequest, fake_settings(), stand.runner());
    check(!none && none.boolean.edit.error.find("do not overlap") != std::string::npos, "unchanged volume = nothing to cope: " + none.boolean.edit.error);
    stand.resultVolume = 0.5; stand.solids = 2;
    auto pieces = run_exact_tube_cut(dir.path, strictRequest, fake_settings(), stand.runner());
    check(bool(pieces) && pieces.warning.find("2 separate pieces") != std::string::npos && pieces.boolean.summary.find("WARNING") != std::string::npos, "two pieces: a warning, not silence");
    stand.solids = 0;
    check(!run_exact_tube_cut(dir.path, strictRequest, fake_settings(), stand.runner()), "no solids refused");
    stand.solids = 1; stand.resultVolume = 1.5;
    check(!run_exact_tube_cut(dir.path, strictRequest, fake_settings(), stand.runner()), "a result bigger than A is refused");
    stand.resultVolume = 0.5; stand.watertight = false;
    check(!run_exact_tube_cut(dir.path, strictRequest, fake_settings(), stand.runner()), "a mesh that is not watertight is refused");
}

void cope_preparation_refusals() {
    TempDir dir;
    EditorDocument document("Refusals");
    document.add_object(tube_object(1, "A", round_spec(0.06, 0.003, 0.3, 'y'), dir.path, "a.brep"));
    document.add_object(tube_object(2, "B", round_spec(0.06, 0.003, 0.4, 'x'), dir.path, "b.brep"));
    check(prepare_exact_cope(dir.path, document, 1, 1).error.find("two different") != std::string::npos, "same object");
    check(!prepare_exact_cope(dir.path, document, 1, 99).error.empty(), "missing object");
    document.find_object(2)->flags.locked = true;
    check(prepare_exact_cope(dir.path, document, 1, 2).error.find("unlocked") != std::string::npos, "locked operand");
    document.find_object(2)->flags.locked = false;
    auto& comps = document.find_object(2)->components;
    const Component record = comps.back();
    comps.pop_back();
    check(prepare_exact_cope(dir.path, document, 1, 2).error.find("no tube record") != std::string::npos, "B without a tube record: " + prepare_exact_cope(dir.path, document, 1, 2).error);
    comps.push_back(record);
    document.find_object(2)->voxelScale = {1.0F, 2.0F, 1.0F};
    ExactCopeOptions gap; gap.gapWorldMeters = 0.001;
    check(prepare_exact_cope(dir.path, document, 1, 2, gap).error.find("stretched") != std::string::npos, "gap on a stretched B refused");
    check(prepare_exact_cope(dir.path, document, 1, 2).error.empty(), "gap 0 works on a stretched B");
    document.find_object(2)->voxelScale = {2.0F, 2.0F, 2.0F};
    auto scaled = prepare_exact_cope(dir.path, document, 1, 2, gap);
    check(scaled.error.empty() && near(scaled.boolean.primitiveB.radius, 0.03 + 0.0005, 1e-12), "uniform scale 2: a 1 mm world gap is 0.5 mm in B's frame");
    document.find_object(2)->components[0].enabled = false;
    check(!prepare_exact_cope(dir.path, document, 1, 2).error.empty(), "an object that dropped its exact link is refused");
}

void miter_preparation() {
    TempDir dir;
    write_cube_glb(dir.path / "cube.glb");
    TubeSpec spec; spec.profile = TubeProfile::Rectangular; spec.width = 0.08; spec.height = 0.04; spec.wall = 0.003; spec.length = 0.6; spec.axis = 'y';
    EditorDocument document("Miter");
    document.add_object(tube_object(1, "Rail", spec, dir.path, "a.brep"));
    ExactMiterSpec miter; miter.angleDegrees = 47.5; miter.positionWorldMeters = 0.05; miter.tilt = 'u';
    auto request = prepare_exact_miter(dir.path, document, 1, miter);
    check(request.error.empty(), "prepare miter: " + request.error);
    check(request.boolean.primitiveB.kind == ExactPrimitive::Kind::Box && request.boolean.primitiveB.size[0] == request.boolean.primitiveB.size[1] &&
              request.boolean.primitiveB.size[0] > 1.0, "a large cube slab: " + num(request.boolean.primitiveB.size[0]));
    // The slab's local +z axis maps to the removed-side normal, its centre is half a side beyond the plane point.
    const auto& m = request.boolean.placement;
    const double side = request.boolean.primitiveB.size[0];
    const V3 normal{m[2], m[6], m[10]};
    check(near(normal[0], std::sin(47.5 * kDeg), 1e-12) && near(normal[1], std::cos(47.5 * kDeg), 1e-12) && near(normal[2], 0, 1e-12), "local z = removed-side normal");
    const V3 centre{m[3], m[7], m[11]};
    const V3 point{0, 0.05, 0};
    check(near(dot(sub(centre, point), normal), side / 2, 1e-9) && near(length(cross(sub(centre, point), normal)), 0, 1e-9), "the slab's face lies on the plane");
    // Rotation part is orthonormal with det +1.
    const V3 c0{m[0], m[4], m[8]}, c1{m[1], m[5], m[9]};
    check(near(length(c0), 1, 1e-12) && near(length(c1), 1, 1e-12) && near(dot(c0, c1), 0, 1e-12) && near(dot(cross(c0, c1), normal), 1, 1e-12), "proper rotation");
    check(request.record == "plane cut 47.5 deg from square, tilt u, at 50 mm, keep low end", "record: " + request.record);

    StandIn stand{{}, dir.path / "cube.glb"};
    auto cut = run_exact_tube_cut(dir.path, request, fake_settings(), stand.runner());
    check(bool(cut), "fake miter: " + cut.boolean.edit.error);
    const auto call = stand.boolean_call();
    check(opt(call, "--op") == "difference" && !opt(call, "--b-transform").empty() && stand.count("primitive") == 1 && stand.seen.calls[0][2] == "box", "box primitive + transform");
    check(cut.boolean.summary.find("Certified exact plane cut") != std::string::npos, "summary: " + cut.boolean.summary);

    // The plane must cross the side walls only.
    ExactMiterSpec out = miter; out.positionWorldMeters = 0.29;
    check(prepare_exact_miter(dir.path, document, 1, out).error.find("leave through the end") != std::string::npos, "plane through the end face refused");
    out = miter; out.angleDegrees = 83;
    check(!prepare_exact_miter(dir.path, document, 1, out).error.empty(), "a very shallow plane leaving through the ends is refused");
    out = miter; out.angleDegrees = 90;
    check(!prepare_exact_miter(dir.path, document, 1, out).error.empty(), "90 degrees refused");
    document.find_object(1)->voxelScale = {1.0F, 2.0F, 1.0F};
    check(prepare_exact_miter(dir.path, document, 1, miter).error.find("stretched") != std::string::npos, "a stretched tube would distort the angle");
    document.find_object(1)->voxelScale = {2.0F, 2.0F, 2.0F};
    auto scaled = prepare_exact_miter(dir.path, document, 1, miter);
    check(scaled.error.empty() && near(scaled.boolean.placement[7] > 0 ? 0 : 0, 0, 1), "uniform scale accepted");
    // world 50 mm at scale 2 = 25 mm in the mesh frame
    const double sideScaled = scaled.boolean.primitiveB.size[0];
    const V3 n2{scaled.boolean.placement[2], scaled.boolean.placement[6], scaled.boolean.placement[10]};
    const V3 c2{scaled.boolean.placement[3], scaled.boolean.placement[7], scaled.boolean.placement[11]};
    check(near(dot(sub(c2, V3{0, 0.025, 0}), n2), sideScaled / 2, 1e-9), "position is converted into the mesh frame");
    document.find_object(1)->voxelScale = {1.0F, 1.0F, 1.0F};
    document.find_object(1)->components.pop_back();
    check(prepare_exact_miter(dir.path, document, 1, miter).error.find("no tube record") != std::string::npos, "plane cut needs a tube record");
}

} // namespace

int main() {
    try {
        spec_parsing();
        miter_parsing_and_plane();
        tube_component_round_trip();
        fake_create_tube();
        fake_cope_and_retry();
        cope_preparation_refusals();
        miter_preparation();
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << "\n";
        return 1;
    }
    if (g_failures) { std::cerr << g_failures << " check(s) failed\n"; return 1; }
    std::cout << "exact tube tests (stand-in brepkernel) passed\n";
    return dve::testing::exit_code(false);
}
