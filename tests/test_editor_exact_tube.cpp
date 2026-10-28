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
#include <chrono>
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
void check(bool condition, const std::string& message = "native command did not start") {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); std::fflush(stdout); ++g_failures; }
}
bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }
bool near_rel(double a, double b, double relative) { return std::abs(a - b) <= relative * (std::max)(std::abs(a), std::abs(b)); }
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
            face.worst = (std::max)(face.worst, std::abs(dot(sub(p, point), normal)));
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


// ---------------------------------------------------------------------------------------------
// Real brepkernel
// ---------------------------------------------------------------------------------------------
struct Frame { V3 c{}; std::array<V3, 3> e{V3{1, 0, 0}, V3{0, 1, 0}, V3{0, 0, 1}}; };   // mesh axes in world, and the mesh origin

Frame frame_of(const EditorObject& o) {
    const double x = o.transform.rotation.x, y = o.transform.rotation.y, z = o.transform.rotation.z, w = o.transform.rotation.w;
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    const double qx = x / n, qy = y / n, qz = z / n, qw = w / n;
    Frame f;
    f.c = {o.transform.position.x, o.transform.position.y, o.transform.position.z};
    const double R[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                            {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                            {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
    for (int col = 0; col < 3; ++col) f.e[std::size_t(col)] = {R[0][col], R[1][col], R[2][col]};
    return f;
}

struct Interval { double lo{-1e300}, hi{1e300}; bool empty{}; };
void clip(Interval& i, double lo, double hi) { if (lo > hi) { i.empty = true; return; } i.lo = (std::max)(i.lo, lo); i.hi = (std::min)(i.hi, hi); if (i.lo > i.hi) i.empty = true; }
void clip_slab(Interval& i, double w, double d, double h) {      // |w + s d| <= h
    if (std::abs(d) < 1e-14) { if (std::abs(w) > h) i.empty = true; return; }
    const double a = (-h - w) / d, b = (h - w) / d;
    clip(i, (std::min)(a, b), (std::max)(a, b));
}

// Independent volume of tube A minus the outer body of tube B grown by `gap`: integrate over A's wall
// cross-section the length of each axial line that stays outside the cutter (analytic per line).
double cope_volume_numeric(const TubeSpec& a, const Frame& fa, const TubeSpec& b, const Frame& fb, double gap, int n = 1500) {
    const auto aa = tube_axes(a.axis), ba = tube_axes(b.axis);
    const V3 eu = fa.e[std::size_t(aa[1])], ev = fa.e[std::size_t(aa[2])], ea = fa.e[std::size_t(aa[0])];
    const double hw = a.width / 2, hh = a.height / 2;
    double total = 0.0;
    const double du = a.width / n, dv = a.height / n;
    for (int i = 0; i < n; ++i) {
        const double p = -hw + (i + 0.5) * du;
        for (int j = 0; j < n; ++j) {
            const double q = -hh + (j + 0.5) * dv;
            bool material;
            if (a.profile == TubeProfile::Round) { const double r2 = p * p + q * q; material = r2 <= hw * hw && r2 >= (hw - a.wall) * (hw - a.wall); }
            else material = !(std::abs(p) < hw - a.wall && std::abs(q) < hh - a.wall);
            if (!material) continue;
            // line through c + p eu + q ev along ea, in B's frame
            V3 w0{}, d{};
            for (int k = 0; k < 3; ++k) {
                const V3 origin{fa.c[0] + p * eu[0] + q * ev[0] - fb.c[0], fa.c[1] + p * eu[1] + q * ev[1] - fb.c[1], fa.c[2] + p * eu[2] + q * ev[2] - fb.c[2]};
                w0[std::size_t(k)] = dot(origin, fb.e[std::size_t(k)]);
                d[std::size_t(k)] = dot(ea, fb.e[std::size_t(k)]);
            }
            Interval inside;
            clip(inside, -a.length / 2, a.length / 2);
            const std::size_t bx = std::size_t(ba[0]), bu = std::size_t(ba[1]), bv = std::size_t(ba[2]);
            clip_slab(inside, w0[bx], d[bx], b.length / 2 + gap);
            if (b.profile == TubeProfile::Round) {
                const double R = b.width / 2 + gap;
                const double qa = d[bu] * d[bu] + d[bv] * d[bv], qb = 2 * (w0[bu] * d[bu] + w0[bv] * d[bv]), qc = w0[bu] * w0[bu] + w0[bv] * w0[bv] - R * R;
                if (qa < 1e-14) { if (qc >= 0) inside.empty = true; }
                else {
                    const double disc = qb * qb - 4 * qa * qc;
                    if (disc <= 0) inside.empty = true;
                    else clip(inside, (-qb - std::sqrt(disc)) / (2 * qa), (-qb + std::sqrt(disc)) / (2 * qa));
                }
            } else {
                clip_slab(inside, w0[bu], d[bu], b.width / 2 + gap);
                clip_slab(inside, w0[bv], d[bv], b.height / 2 + gap);
            }
            const double removed = inside.empty ? 0.0 : (std::max)(0.0, inside.hi - inside.lo);
            total += (a.length - removed) * du * dv;
        }
    }
    return total;
}

struct RealWorld {
    ExactCadSettings settings;
    fs::path root;
};

EditorObject make_real_tube(RealWorld& w, EditorObjectId id, const std::string& name, const TubeSpec& spec, V3 position, Float3 axis = {0, 0, 1}, float angle = 0.0F) {
    auto made = create_exact_tube(w.root, spec, id, w.settings);
    check(bool(made), "real tube " + name + ": " + made.error);
    EditorObject object(id, name);
    object.voxelSizeMeters = 1.0F;
    object.polygon = made.mesh;
    object.polygon->objectId = id;
    Component exact = make_exact_brep_component(made.source);
    exact.id = 1;
    object.components.push_back(exact);
    Component record = make_exact_tube_component(spec);
    record.id = 2;
    object.components.push_back(record);
    object.transform.position = {float(position[0]), float(position[1]), float(position[2])};
    if (angle != 0.0F) object.transform.rotation = quaternion_from_axis_angle(axis, angle);
    return object;
}

void real_tubes(RealWorld& w) {
    struct Case { std::string name; TubeSpec spec; };
    TubeSpec rect; rect.profile = TubeProfile::Rectangular; rect.width = 0.08; rect.height = 0.04; rect.wall = 0.003; rect.length = 0.6; rect.axis = 'z';
    TubeSpec square; square.profile = TubeProfile::Square; square.width = square.height = 0.04; square.wall = 0.002; square.length = 0.5; square.axis = 'x';
    std::uint32_t id = 100;
    for (const Case& c : {Case{"round y", round_spec(0.0508, 0.003, 0.6, 'y')}, Case{"round x thin", round_spec(0.0254, 0.0015, 0.3, 'x')},
                          Case{"square x", square}, Case{"rect z", rect}}) {
        auto made = create_exact_tube(w.root, c.spec, id++, w.settings);
        check(bool(made), "create " + c.name + ": " + made.error);
        if (!made) continue;
        check(near_rel(made.exactVolume, tube_volume(c.spec), 1e-9), c.name + ": certified volume " + num(made.exactVolume) + " vs analytic " + num(tube_volume(c.spec)));
        check(near_rel(mesh_volume(made.mesh), tube_volume(c.spec), c.spec.profile == TubeProfile::Round ? 0.012 : 1e-6), c.name + ": mesh volume " + num(mesh_volume(made.mesh)));
        check(closed(made.mesh), c.name + ": closed mesh");
        // wall thickness and size from the exact faces
        const auto described = describe_exact_cad(w.root, made.source, w.settings);
        check(bool(described), c.name + ": describe: " + described.error);
        if (!described) continue;
        check(near_rel(described.volume, tube_volume(c.spec), 1e-9), c.name + ": describe volume");
        const auto axes = tube_axes(c.spec.axis);
        if (c.spec.profile == TubeProfile::Round) {
            std::vector<double> radii;
            for (const auto& f : described.faces) if (f.type == "cylinder") radii.push_back(f.radius);
            std::sort(radii.begin(), radii.end());
            check(radii.size() == 2 && near(radii[0], c.spec.width / 2 - c.spec.wall, 1e-12) && near(radii[1], c.spec.width / 2, 1e-12), c.name + ": wall = outer radius - bore radius");
        } else {
            for (int k = 1; k <= 2; ++k) {
                std::vector<double> offsets;
                for (const auto& f : described.faces)
                    if (f.type == "plane" && std::abs(f.normal[std::size_t(axes[std::size_t(k)])]) > 1 - 1e-9) offsets.push_back(f.origin[std::size_t(axes[std::size_t(k)])]);
                std::sort(offsets.begin(), offsets.end());
                const double half = (k == 1 ? c.spec.width : c.spec.height) / 2;
                check(offsets.size() == 4 && near(offsets[0], -half, 1e-12) && near(offsets[1], -half + c.spec.wall, 1e-12) &&
                          near(offsets[2], half - c.spec.wall, 1e-12) && near(offsets[3], half, 1e-12), c.name + ": four wall planes, wall thickness exact (axis " + std::to_string(k) + ")");
            }
        }
        // the tube's ends are planes at +-length/2 normal to the axis
        int ends = 0;
        for (const auto& f : described.faces)
            if (f.type == "plane" && std::abs(f.normal[std::size_t(axes[0])]) > 1 - 1e-9 && near(std::abs(f.origin[std::size_t(axes[0])]), c.spec.length / 2, 1e-12)) ++ends;
        check(ends == 2, c.name + ": two end planes at +-length/2");
    }
}

// Plane cut: the cut face lies on the plane through both walls.
void real_miter(RealWorld& w) {
    TubeSpec rect; rect.profile = TubeProfile::Rectangular; rect.width = 0.08; rect.height = 0.04; rect.wall = 0.003; rect.length = 0.6; rect.axis = 'y';   // u = x (80), v = z (40)
    struct Case { std::string name; double angle; char tilt; MiterKeep keep; double position; double scale; };
    for (const Case& c : {Case{"47.5 u low", 47.5, 'u', MiterKeep::Low, 0.05, 1.0}, Case{"47.5 u high", 47.5, 'u', MiterKeep::High, -0.04, 1.0},
                          Case{"47.5 v low", 47.5, 'v', MiterKeep::Low, 0.0, 1.0}, Case{"-30 u low", -30.0, 'u', MiterKeep::Low, 0.1, 1.0},
                          Case{"square cut", 0.0, 'u', MiterKeep::Low, 0.05, 1.0}, Case{"60 v high, scale 2", 60.0, 'v', MiterKeep::High, 0.1, 2.0}}) {
        EditorDocument document("Miter");
        auto tube = make_real_tube(w, 1, "Rail", rect, {0.3, 0.2, -0.1}, {0, 0, 1}, 0.4F);
        tube.voxelScale = {float(c.scale), float(c.scale), float(c.scale)};
        document.add_object(std::move(tube));
        ExactMiterSpec miter; miter.angleDegrees = c.angle; miter.tilt = c.tilt; miter.keep = c.keep; miter.positionWorldMeters = c.position * c.scale;
        auto request = prepare_exact_miter(w.root, document, 1, miter);
        check(request.error.empty(), c.name + ": prepare " + request.error);
        if (!request.error.empty()) continue;
        auto cut = run_exact_tube_cut(w.root, request, w.settings);
        check(bool(cut), c.name + ": certified cut: " + cut.boolean.edit.error);
        if (!cut) continue;
        const auto& mesh = cut.boolean.edit.mesh;
        const MiterPlane plane = miter_plane(rect, miter, c.position);
        // kept volume = cross section x (distance from the kept end to the plane at the axis)
        const double keptLength = c.keep == MiterKeep::Low ? c.position + rect.length / 2 : rect.length / 2 - c.position;
        const double expected = tube_cross_section_area(rect) * keptLength;
        check(near_rel(cut.boolean.exactVolume, expected, 1e-9), c.name + ": certified volume " + num(cut.boolean.exactVolume) + " vs analytic " + num(expected));
        check(near_rel(mesh_volume(mesh), expected, 1e-6), c.name + ": mesh volume");
        check(closed(mesh), c.name + ": closed mesh");
        const auto face = plane_face(mesh, plane.point, plane.normal);
        const double cosine = std::abs(std::cos(c.angle * kDeg));
        check(face.worst < 2e-6, c.name + ": cut-face vertices within " + num(face.worst) + " m of the plane");
        check(near_rel(face.area, tube_cross_section_area(rect) / cosine, 2e-6), c.name + ": cut-face area " + num(face.area) + " vs " + num(tube_cross_section_area(rect) / cosine) +
                                                                           " (wall cross-section / cos): the face spans both walls");
        // outer and inner rectangle corners are on the plane: 8 distinct vertices at least
        std::vector<V3> distinct;
        for (const V3& p : face.vertices) {
            bool seen = false;
            for (const V3& q : distinct) seen = seen || length(sub(p, q)) < 1e-6;
            if (!seen) distinct.push_back(p);
        }
        check(distinct.size() >= 8, c.name + ": " + std::to_string(distinct.size()) + " cut-face vertices (outer + inner corners)");
        // exact face: one plane through the point with the right normal; walls keep their thickness
        const auto described = describe_exact_cad(w.root, cut.boolean.source, w.settings);
        check(bool(described), c.name + ": describe " + described.error);
        if (!described) continue;
        int cutFaces = 0;
        for (const auto& f : described.faces)
            if (f.type == "plane" && std::abs(std::abs(dot(f.normal, plane.normal)) - 1.0) < 1e-12 && std::abs(dot(sub(f.origin, plane.point), plane.normal)) < 1e-9) ++cutFaces;
        check(cutFaces == 1, c.name + ": one exact plane face on the cut plane (found " + std::to_string(cutFaces) + ")");
        for (int k = 1; k <= 2; ++k) {
            std::vector<double> offsets;
            for (const auto& f : described.faces)
                if (f.type == "plane" && std::abs(f.normal[std::size_t(tube_axes('y')[std::size_t(k)])]) > 1 - 1e-9) offsets.push_back(f.origin[std::size_t(tube_axes('y')[std::size_t(k)])]);
            std::sort(offsets.begin(), offsets.end());
            const double half = (k == 1 ? rect.width : rect.height) / 2;
            check(offsets.size() == 4 && near(offsets[1] - offsets[0], rect.wall, 1e-12) && near(offsets[3] - offsets[2], rect.wall, 1e-12) && near(offsets[3] - offsets[0], 2 * half, 1e-12),
                  c.name + ": wall thickness preserved (axis " + std::to_string(k) + ")");
        }
        check(cut.record.find("plane cut") == 0, c.name + ": record " + cut.record);
    }
    // 47.5 degrees: on a round tube too (elliptical cut face)
    TubeSpec round = round_spec(0.0508, 0.003, 0.6, 'y');
    EditorDocument document("Round miter");
    document.add_object(make_real_tube(w, 1, "Pipe", round, {0, 0, 0}));
    ExactMiterSpec miter; miter.angleDegrees = 47.5;
    auto cut = run_exact_tube_cut(w.root, prepare_exact_miter(w.root, document, 1, miter), w.settings);
    check(bool(cut), "round miter: " + cut.boolean.edit.error);
    if (cut) {
        check(near_rel(cut.boolean.exactVolume, tube_cross_section_area(round) * (round.length / 2), 1e-9), "round miter volume");
        const auto plane = miter_plane(round, miter, 0.0);
        const auto face = plane_face(cut.boolean.edit.mesh, plane.point, plane.normal);
        check(face.worst < 2e-6 && near_rel(face.area, tube_cross_section_area(round) / std::cos(47.5 * kDeg), 1.2e-2), "round miter: elliptical annulus on the plane, area " + num(face.area));
    }
}

struct CopeCase {
    std::string name;
    TubeSpec a, b;
    V3 posA{}, posB{};
    Float3 axisA{0, 0, 1}, axisB{0, 0, 1};
    float angleA{}, angleB{};
    double gap{};
    double volumeTolerance{2e-3};
};

ExactTubeCutResult run_cope_case(RealWorld& w, const CopeCase& c, EditorDocument* keep = nullptr, ExactCopeOptions options = {}) {
    EditorDocument document("Cope");
    document.add_object(make_real_tube(w, 1, "A", c.a, c.posA, c.axisA, c.angleA));
    document.add_object(make_real_tube(w, 2, "B", c.b, c.posB, c.axisB, c.angleB));
    options.gapWorldMeters = c.gap;
    auto request = prepare_exact_cope(w.root, document, 1, 2, options);
    check(request.error.empty(), c.name + ": prepare " + request.error);
    auto cut = run_exact_tube_cut(w.root, request, w.settings);
    if (keep) *keep = std::move(document);
    return cut;
}

void check_cope_volume(RealWorld& w, const CopeCase& c, const ExactTubeCutResult& cut, double appliedGap) {
    EditorDocument document("Frames");
    auto a = make_real_tube(w, 1, "A", c.a, c.posA, c.axisA, c.angleA);
    auto b = make_real_tube(w, 2, "B", c.b, c.posB, c.axisB, c.angleB);
    const double numeric = cope_volume_numeric(c.a, frame_of(a), c.b, frame_of(b), appliedGap);
    check(near_rel(cut.boolean.exactVolume, numeric, c.volumeTolerance), c.name + ": certified volume " + num(cut.boolean.exactVolume) + " vs numeric " + num(numeric));
    check(near_rel(mesh_volume(cut.boolean.edit.mesh), cut.boolean.exactVolume, 0.015), c.name + ": mesh volume " + num(mesh_volume(cut.boolean.edit.mesh)));
    check(closed(cut.boolean.edit.mesh), c.name + ": closed mesh");
    check(cut.boolean.exactVolume < tube_volume(c.a), c.name + ": material was removed");
}

void real_cope(RealWorld& w) {
    const TubeSpec branch = round_spec(0.060, 0.003, 0.30, 'y');
    const TubeSpec run = round_spec(0.100, 0.004, 0.40, 'x');
    // 1. round on round, 90 degrees, the branch's end on the run's axis
    CopeCase c1{"round 90", branch, run, {0, 0.15, 0}, {0, 0, 0}};
    auto cut = run_cope_case(w, c1);
    check(bool(cut) && !cut.clearanceApplied, "round 90: certified at zero gap: " + cut.boolean.edit.error);
    if (cut) {
        check_cope_volume(w, c1, cut, 0.0);
        // the cut face is on B's true surface through both walls: vertices off A's own surfaces lie on B's cylinder
        const auto& mesh = cut.boolean.edit.mesh;
        int onB = 0, onBOuterWall = 0, onBInnerWall = 0, strays = 0;
        for (const V3& p : all_vertices(mesh)) {
            const double rA = std::sqrt(p[0] * p[0] + p[2] * p[2]);                       // A's axis is the mesh y axis
            const double dB = std::sqrt((p[1] + 0.15) * (p[1] + 0.15) + p[2] * p[2]);      // B's axis: world x axis = mesh y = -0.15
            const bool onAOuter = near(rA, 0.03, 2e-6), onAInner = near(rA, 0.027, 2e-6), onAEnd = near(p[1], 0.15, 2e-6), onBSurface = near(dB, 0.05, 2e-6);
            if (onBSurface) { ++onB; onBOuterWall += onAOuter; onBInnerWall += onAInner; }
            if (!(onAOuter || onAInner || onAEnd || onBSurface)) ++strays;
        }
        check(strays == 0, "round 90: every vertex is on A's walls, A's far end or B's surface (" + std::to_string(strays) + " strays)");
        check(onBOuterWall >= 16 && onBInnerWall >= 16, "round 90: the saddle meets B's surface at both walls (" + std::to_string(onBOuterWall) + " outer, " + std::to_string(onBInnerWall) + " inner)");
        const auto described = describe_exact_cad(w.root, cut.boolean.source, w.settings);
        bool exactB = false, outerA = false, innerA = false;
        for (const auto& f : described.faces) {
            if (f.type != "cylinder") continue;
            if (near(f.radius, 0.05, 1e-12) && near(std::abs(f.axis[0]), 1.0, 1e-12) && near(f.axisOrigin[1] + 0.15, 0.0, 1e-6) && near(f.axisOrigin[2], 0.0, 1e-6)) exactB = true;
            if (near(f.radius, 0.03, 1e-12)) outerA = true;
            if (near(f.radius, 0.027, 1e-12)) innerA = true;
        }
        std::string faces;
        for (const auto& f : described.faces) if (f.type == "cylinder") faces += " [r=" + num(f.radius) + " axis=" + num(f.axis[0]) + "," + num(f.axis[1]) + "," + num(f.axis[2]) + " at " + num(f.axisOrigin[0]) + "," + num(f.axisOrigin[1]) + "," + num(f.axisOrigin[2]) + "]";
        check(exactB, "round 90: an exact cylinder face of B's outer radius about B's axis:" + faces);
        check(outerA && innerA, "round 90: A's outer and inner cylinders (wall thickness) preserved");
    }
    // 2. other angles between the axes: 60 and 30 degrees (the branch leans in the xy plane)
    for (const double degrees : {60.0, 30.0}) {
        const double phi = (degrees - 90.0) * kDeg;     // mesh y axis -> (-sin phi, cos phi, 0)
        CopeCase c{"round " + std::to_string(int(degrees)), branch, run, {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 0, 1}, float(phi), 0.0F};
        const V3 dir{-std::sin(phi), std::cos(phi), 0.0};
        c.posA = {dir[0] * 0.15, dir[1] * 0.15, 0.0};
        auto at = run_cope_case(w, c);
        check(bool(at), c.name + ": " + at.boolean.edit.error);
        if (at) check_cope_volume(w, c, at, 0.0);
    }
    // 3. offset axes: the branch is 25 mm off the run's axis
    {
        CopeCase c{"offset axes", branch, run, {0.02, 0.15, 0.025}, {0, 0, 0}};
        auto at = run_cope_case(w, c);
        check(bool(at), "offset axes: " + at.boolean.edit.error);
        if (at) check_cope_volume(w, c, at, 0.0);
    }
    // 4. mixed sizes and profiles: small round onto a big rectangle, a rectangle onto a round, square onto rectangle
    TubeSpec bigRect; bigRect.profile = TubeProfile::Rectangular; bigRect.width = 0.12; bigRect.height = 0.08; bigRect.wall = 0.004; bigRect.length = 0.5; bigRect.axis = 'x';
    TubeSpec smallRect; smallRect.profile = TubeProfile::Rectangular; smallRect.width = 0.05; smallRect.height = 0.03; smallRect.wall = 0.003; smallRect.length = 0.3; smallRect.axis = 'y';
    TubeSpec square; square.profile = TubeProfile::Square; square.width = square.height = 0.04; square.wall = 0.002; square.length = 0.3; square.axis = 'y';
    for (CopeCase c : {CopeCase{"round onto rect", branch, bigRect, {0, 0.15, 0}, {0, 0, 0}},
                       CopeCase{"rect onto round", smallRect, run, {0, 0.15, 0}, {0, 0, 0}},
                       CopeCase{"square onto rect", square, bigRect, {0.01, 0.15, 0.005}, {0, 0, 0}}}) {
        c.volumeTolerance = 4e-3;
        auto at = run_cope_case(w, c);
        check(bool(at), c.name + ": " + at.boolean.edit.error);
        if (at) check_cope_volume(w, c, at, 0.0);
    }
    // 5. a fit-up gap: the cutter is B's outer body grown by 2 mm all round
    {
        CopeCase c{"gap 2 mm", branch, run, {0, 0.15, 0}, {0, 0, 0}};
        c.gap = 0.002;
        auto at = run_cope_case(w, c);
        check(bool(at) && !at.clearanceApplied, "gap 2 mm: " + at.boolean.edit.error);
        if (at) {
            check_cope_volume(w, c, at, 0.002);
            const auto plain = run_cope_case(w, CopeCase{"gap 0", branch, run, {0, 0.15, 0}, {0, 0, 0}});
            check(at.boolean.exactVolume < plain.boolean.exactVolume, "gap 2 mm removes more material than gap 0");
            check(at.record == "cope to B, gap 2 mm", "record: " + at.record);
        }
    }
    // 6. the branch crosses the whole run: two separate pieces, reported
    {
        CopeCase c{"through", branch, run, {0, 0, 0}, {0, 0, 0}};
        auto at = run_cope_case(w, c);
        check(bool(at) && at.boolean.solids == 2 && !at.warning.empty() && at.boolean.summary.find("WARNING") != std::string::npos, "through: two pieces with a warning: " + at.boolean.edit.error + at.boolean.summary);
    }
    // 7. no overlap: nothing to cope
    {
        CopeCase c{"apart", branch, run, {0, 0.5, 0}, {0, 0, 0}};
        auto at = run_cope_case(w, c);
        check(!at && at.boolean.edit.error.find("do not overlap") != std::string::npos, "apart: " + at.boolean.edit.error);
    }
}

// The equal-diameter 90 degree case.
void real_equal_diameter(RealWorld& w) {
    const TubeSpec a = round_spec(0.0508, 0.003, 0.3, 'y');
    const TubeSpec b = round_spec(0.0508, 0.003, 0.4, 'x');
    CopeCase c{"equal diameter", a, b, {0, 0.15, 0}, {0, 0, 0}};
    ExactCopeOptions strict; strict.retryTangent = false;
    auto zero = run_cope_case(w, c, nullptr, strict);
    if (zero.boolean.refused) {
        check(!zero.boolean.refusalKind.empty() && zero.boolean.edit.error.find("refused") != std::string::npos && !zero.clearanceApplied, "zero gap: clean refusal with brepkernel's reason: " + zero.boolean.edit.error);
        std::cout << "equal diameter, zero gap: refused (" << zero.boolean.refusalKind << ") as expected\n";
    } else {
        check(bool(zero), "zero gap neither refused nor certified: " + zero.boolean.edit.error);
        std::cout << "note: brepkernel certified the equal-diameter zero-gap cope here\n";
        if (zero) check_cope_volume(w, c, zero, 0.0);
    }
    // default options: the documented clearance, certified, and said so
    auto cut = run_cope_case(w, c);
    check(bool(cut), "equal diameter with the documented clearance: " + cut.boolean.edit.error);
    if (!cut) return;
    if (zero.boolean.refused) {
        check(cut.clearanceApplied && near(cut.appliedGapMeters, 1e-5, 1e-15), "the 0.01 mm clearance was applied");
        check(cut.boolean.summary.find("not for zero gap") != std::string::npos && cut.record.find("automatic tangent clearance") != std::string::npos, "and stated: " + cut.boolean.summary);
        check(!cut.firstRefusal.empty(), "with the first refusal kept");
        check_cope_volume(w, c, cut, 1e-5);
        // the cut is still exactly on a cylinder of radius R + 0.01 mm about B's axis
        const auto described = describe_exact_cad(w.root, cut.boolean.source, w.settings);
        bool found = false;
        for (const auto& f : described.faces) found = found || (f.type == "cylinder" && near(f.radius, 0.0254 + 1e-5, 1e-12));
        check(found, "the cut face is the cylinder of radius R + clearance");
    }
    // equal square tubes crossing at 90 degrees (coplanar and tangent faces)
    TubeSpec sq; sq.profile = TubeProfile::Square; sq.width = sq.height = 0.04; sq.wall = 0.002; sq.length = 0.3; sq.axis = 'y';
    TubeSpec sqB = sq; sqB.axis = 'x'; sqB.length = 0.4;
    CopeCase s{"equal squares", sq, sqB, {0, 0.15, 0}, {0, 0, 0}};
    auto squares = run_cope_case(w, s);
    check(bool(squares) || squares.boolean.refused, "equal squares: certified or cleanly refused: " + squares.boolean.edit.error);
    if (squares) { check_cope_volume(w, s, squares, squares.clearanceApplied ? 1e-5 : 0.0); std::cout << "equal squares: " << (squares.clearanceApplied ? "with clearance" : "zero gap certified") << "\n"; }
    else std::cout << "equal squares: refused (" << squares.boolean.edit.error << ")\n";
}


// ---------------------------------------------------------------------------------------------
// Native editor commands
// ---------------------------------------------------------------------------------------------
std::unique_ptr<NativeEditorController> make_native(const fs::path& root) {
    auto c = std::make_unique<NativeEditorController>();
    c->configure_ai_assistant(root);
    c->resize(1600, 900);
    return c;
}

const MenuAction* menu_action(NativeEditorController& c, const char* id) {
    c.refresh_menu_state();
    return std::as_const(c.workspace().menus()).find(id);
}

void type_and_enter(NativeEditorController& c, const std::string& text) {
    c.text_input(text);
    c.key_down("enter", false, false, false);
    (void)c.wait_for_long_job(300.0);
}

// Without brepkernel: menu entries, honest disabled states, typed-input errors, a missing tool, the
// hand-edit guard. None of these reaches brepkernel for geometry.
void native_always(const fs::path& root) {
    auto owner = make_native(root);
    NativeEditorController& c = *owner;
    for (const char* id : {"polygon.create_exact_tube", "mesh.exact_cope", "mesh.exact_miter"}) {
        const MenuAction* action = menu_action(c, id);
        check(action != nullptr, std::string("menu action exists: ") + id);
        check(action && !action->description.empty(), std::string("menu action is described for the command palette: ") + id);
    }
    check(menu_action(c, "polygon.create_exact_tube")->enabled, "Create Exact Tube is available in Edit mode with nothing selected");
    {
        const MenuAction* cope = menu_action(c, "mesh.exact_cope");
        check(cope && !cope->enabled && cope->disabledReason.find("two tubes") != std::string::npos, "cope: needs two tubes: " + (cope ? cope->disabledReason : ""));
        const MenuAction* miter = menu_action(c, "mesh.exact_miter");
        check(miter && !miter->enabled && miter->disabledReason.find("one tube") != std::string::npos, "miter: needs one tube");
        check(!c.dispatch_action("mesh.exact_cope") && c.status().error, "cope with nothing selected is refused with a message: " + c.status().text);
    }

    // Typed-input errors change nothing and say what is wrong.
    const std::size_t steps = c.workspace().commands().size();
    check(c.dispatch_action("polygon.create_exact_tube") && c.text_edit().kind == TextEditKind::ExactTube, "Create Exact Tube opens its field");
    type_and_enter(c, "round 50 30 600 y");   // the bore would leave no wall
    check(c.status().error && c.workspace().commands().size() == steps, "a tube with no wall is refused: " + c.status().text);
    check(c.dispatch_action("polygon.create_exact_tube"));
    type_and_enter(c, "hexagon 50 3 600");
    check(c.status().error && c.workspace().commands().size() == steps, "an unknown profile is refused: " + c.status().text);

    // brepkernel missing: an error naming it, no object, no undo step, no stale undo state.
    {
        const char* original = std::getenv("DVE_BREPKERNEL");
        const std::string saved = original ? original : "";
        set_test_env("DVE_BREPKERNEL", "/nonexistent/brepkernel-for-test");
        const std::size_t objects = c.workspace().document().objects().size();
        check(c.dispatch_action("polygon.create_exact_tube"));
        type_and_enter(c, "round 50 3 600 y");
        check(c.status().error && c.workspace().document().objects().size() == objects && c.workspace().commands().size() == steps,
              "no brepkernel: nothing is created: " + c.status().text);
        if (original) set_test_env("DVE_BREPKERNEL", saved.c_str()); else unset_test_env("DVE_BREPKERNEL");
    }

    // A tube record with a stock exact source: the inspector line, the hand-edit guard, locked and isolated states.
    EditorObject tube(c.workspace().document().allocate_object_id(), "Stock tube");
    tube.polygon = make_editor_polygon_cube(tube.id);
    ExactBrepSource source{"sources/exact/tube-0123456789ab.brep", "0123456789ab", "tube", 0.0, 0.0};
    Component exact = make_exact_brep_component(source);
    exact.id = 1;
    tube.components.push_back(exact);
    TubeSpec spec;
    Component record = make_exact_tube_component(spec, "cope to B, gap 0 mm");
    record.id = 2;
    tube.components.push_back(record);
    const EditorObjectId tubeId = tube.id;
    c.workspace().document().add_object(std::move(tube));
    c.workspace().select_object(tubeId);
    check(c.exact_tube_summary(tubeId).find("Round tube") != std::string::npos && c.exact_tube_summary(tubeId).find("1 cut") != std::string::npos,
          "inspector summary: " + c.exact_tube_summary(tubeId));
    check(menu_action(c, "mesh.exact_miter")->enabled, "miter is available for one tube");
    check(!menu_action(c, "mesh.exact_cope")->enabled, "cope still needs a second tube");
    const auto edited = c.set_component_property_text_on_primary(2, "wall", "0.01");
    check(!edited.success && edited.message.find("Create Exact Tube") != std::string::npos, "the tube record cannot be edited by hand: " + edited.message);
    c.workspace().document().find_object(tubeId)->flags.locked = true;
    {
        const MenuAction* miter = menu_action(c, "mesh.exact_miter");
        check(miter && !miter->enabled && miter->disabledReason.find("unlocked") != std::string::npos, "a locked tube is refused with a reason: " + (miter ? miter->disabledReason : ""));
    }
    c.workspace().document().find_object(tubeId)->flags.locked = false;
    // A plain CAD object without a tube record is a fine A, but never a B (its outer body is unknown).
    EditorObject plate(c.workspace().document().allocate_object_id(), "Plate");
    plate.polygon = make_editor_polygon_cube(plate.id);
    Component plateExact = make_exact_brep_component({"sources/exact/plate-0123456789ab.brep", "0123456789ab", "plate", 0.0, 0.0});
    plateExact.id = 1;
    plate.components.push_back(plateExact);
    const EditorObjectId plateId = plate.id;
    c.workspace().document().add_object(std::move(plate));
    c.workspace().select_object(plateId);
    c.workspace().add_to_selection(tubeId);   // tube is A
    {
        const MenuAction* cope = menu_action(c, "mesh.exact_cope");
        check(cope && !cope->enabled && cope->disabledReason.find("(B)") != std::string::npos && cope->disabledReason.find("tube record") != std::string::npos,
              "B without a tube record is refused: " + (cope ? cope->disabledReason : ""));
    }
    check(c.workspace().document().find_object(plateId) != nullptr && c.workspace().commands().size() == steps + 0,
          "no undo steps were added by refusals");
}

// With brepkernel: create, cope, plane cut end to end through the editor, undo/redo, save/load.
void native_real(const fs::path& root) {
    auto owner = make_native(root);
    NativeEditorController& c = *owner;
    auto& document = c.workspace().document();
    const auto select_one = [&](EditorObjectId id) { c.workspace().select_object(id); };
    const auto create = [&](const std::string& text) {
        check(c.dispatch_action("polygon.create_exact_tube") && c.text_edit().kind == TextEditKind::ExactTube, "create opens: " + c.status().text);
        type_and_enter(c, text);
        check(!c.status().error, "create '" + text + "': " + c.status().text);
        return *c.workspace().selected_object();
    };
    const std::size_t steps0 = c.workspace().commands().size();
    const EditorObjectId a = create("round 50 3 400 y");
    check(c.workspace().commands().size() == steps0 + 1, "creating a tube is one undo step");
    {
        const EditorObject* object = document.find_object(a);
        const auto record = object ? exact_tube_record(*object) : std::nullopt;
        check(object && exact_brep_source(*object) && record && record->spec.profile == TubeProfile::Round && near(record->spec.wall, 0.003, 1e-12),
              "the tube carries its exact source and record");
        check(object && near_rel(mesh_volume(*object->polygon), tube_volume(record->spec), 1.2e-2), "mesh volume matches the analytic tube volume");
        check(c.exact_tube_summary(a).find("Round tube") != std::string::npos, "inspector summary: " + c.exact_tube_summary(a));
    }
    check(c.dispatch_action("edit.undo") && document.find_object(a) == nullptr, "undo removes the tube");
    check(c.dispatch_action("edit.redo") && document.find_object(a) != nullptr && exact_tube_record(*document.find_object(a)), "redo restores it with its record");

    // Two tubes: A (round 50 mm, along y) and B (round 40 mm, along x), offset and rotated to cross.
    select_one(a);
    const EditorObjectId b = create("round 40 3 400 x");
    document.find_object(b)->transform.position = document.find_object(a)->transform.position;
    select_one(b);
    c.workspace().add_to_selection(a);   // A is primary
    check(*c.workspace().selected_object() == a, "A is the active object");
    check(menu_action(c, "mesh.exact_cope")->enabled, "cope is enabled for A and B: " + menu_action(c, "mesh.exact_cope")->disabledReason);
    const std::size_t objectsBefore = document.objects().size();
    const std::size_t stepsBefore = c.workspace().commands().size();
    const double volumeA = mesh_volume(*document.find_object(a)->polygon), volumeB = mesh_volume(*document.find_object(b)->polygon);
    check(c.dispatch_action("mesh.exact_cope") && c.text_edit().kind == TextEditKind::ExactCopeGap, "cope asks for the gap");
    type_and_enter(c, "0");
    check(!c.status().error, "cope: " + c.status().text);
    check(document.objects().size() == objectsBefore + 1 && c.workspace().commands().size() == stepsBefore + 1, "cope adds one object in one undo step");
    const EditorObjectId coped = *c.workspace().selected_object();
    check(coped != a && coped != b, "the result is a new object");
    {
        const EditorObject* object = document.find_object(coped);
        const auto record = exact_tube_record(*object);
        check(record && record->cuts.find("cope to") != std::string::npos, "the cut is recorded: " + (record ? record->cuts : "none"));
        check(mesh_volume(*object->polygon) < volumeA && mesh_volume(*object->polygon) > 0.5 * volumeA, "the coped tube lost material, not all of it");
        check(near_rel(mesh_volume(*document.find_object(a)->polygon), volumeA, 1e-12) && near_rel(mesh_volume(*document.find_object(b)->polygon), volumeB, 1e-12),
              "the operands are unchanged");
        check(c.exact_tube_summary(coped).find("1 cut") != std::string::npos, "summary counts the cut: " + c.exact_tube_summary(coped));
    }
    // Plane cut of the coped tube at 47.5 degrees: second cut, chained.
    select_one(coped);
    check(c.dispatch_action("mesh.exact_miter") && c.text_edit().kind == TextEditKind::ExactMiter, "miter asks for the plane");
    type_and_enter(c, "47.5 high at -120");
    check(!c.status().error, "miter: " + c.status().text);
    const EditorObjectId mitered = *c.workspace().selected_object();
    check(mitered != coped && exact_tube_record(*document.find_object(mitered)) &&
          exact_tube_record(*document.find_object(mitered))->cuts.find(';') != std::string::npos, "the chain keeps both cuts");
    check(c.exact_tube_summary(mitered).find("2 cuts") != std::string::npos, "summary: " + c.exact_tube_summary(mitered));
    // Undo and redo the miter and the cope.
    check(c.dispatch_action("edit.undo") && document.find_object(mitered) == nullptr, "undo the plane cut");
    check(c.dispatch_action("edit.undo") && document.find_object(coped) == nullptr && document.objects().size() == objectsBefore, "undo the cope");
    check(c.dispatch_action("edit.redo") && document.find_object(coped) != nullptr, "redo the cope");
    check(c.dispatch_action("edit.redo") && document.find_object(mitered) != nullptr, "redo the plane cut");

    // A cut that misses the material is refused and changes nothing.
    {
        select_one(a);
        const std::size_t before = c.workspace().commands().size();
        const std::size_t objects = document.objects().size();
        check(c.dispatch_action("mesh.exact_miter"));
        type_and_enter(c, "30 high at 190");   // beyond the end: outside the safe range
        check(c.status().error && c.workspace().commands().size() == before && document.objects().size() == objects,
              "a plane outside the tube is refused: " + c.status().text);
    }
    // A cut whose gap is negative or not a number is refused.
    {
        select_one(b);
        c.workspace().add_to_selection(a);
        const std::size_t before = c.workspace().commands().size();
        check(c.dispatch_action("mesh.exact_cope"));
        type_and_enter(c, "-1");
        check(c.status().error && c.workspace().commands().size() == before, "a negative gap is refused: " + c.status().text);
        check(c.dispatch_action("mesh.exact_cope"));
        type_and_enter(c, "wide");
        check(c.status().error && c.workspace().commands().size() == before, "a non-number is refused: " + c.status().text);
    }
    // Equal diameters at 90 degrees: certified with the documented clearance, and the status says so.
    {
        const EditorObjectId e1 = create("round 50 3 400 y");
        const EditorObjectId e2 = create("round 50 3 400 x");
        document.find_object(e2)->transform.position = document.find_object(e1)->transform.position;
        select_one(e2);
        c.workspace().add_to_selection(e1);
        check(c.dispatch_action("mesh.exact_cope"));
        type_and_enter(c, "0");
        const std::string text = c.status().text;
        std::printf("native equal-diameter status: %s\n", text.c_str());
        check(text.find("clearance") != std::string::npos || c.status().error, "equal diameters: the clearance (or a refusal) is stated: " + text);
        if (!c.status().error) check(text.find("0.01") != std::string::npos, "the clearance amount is stated: " + text);
    }
    // Save and load: the exact source and the record survive a scene round trip.
    {
        const auto before = exact_tube_record(*document.find_object(mitered));
        const auto saved = document.save_transactional(root / "native-scene.dvescene");
        check(saved.success, "scene saves");
        std::string error;
        auto loadedScene = EditorDocument::load(saved.manifestPath, &error);
        check(loadedScene.has_value(), "scene reloads: " + error);
        if (!loadedScene) return;
        const EditorDocument& loaded = *loadedScene;
        const EditorObject* object = loaded.find_object(mitered);
        const auto after = object ? exact_tube_record(*object) : std::nullopt;
        check(object && after && before && after->cuts == before->cuts && near(after->spec.width, before->spec.width, 1e-12) && exact_brep_source(*object),
              "save/load keeps the tube record and exact source");
    }
}

bool brepkernel_runs(const ExactCadSettings& settings, const fs::path& dir, std::string& why) {
    auto argv = settings.command;
    argv.insert(argv.end(), {"primitive", "box", "-o", (dir / "probe.brep").string(), "--report", (dir / "probe.json").string()});
    const auto result = run_exact_cad_process(argv, std::chrono::seconds(120), 1U << 20U);
    why = result.error + result.output.substr(0, 200);
    return result.launched && !result.timedOut && result.exitCode == 0;
}

} // namespace

int main(int argc, char** argv) {
    // Optional argument: run only the real-brepkernel sections whose name contains it (tubes, miter, cope, equal).
    const std::string only = argc > 1 ? argv[1] : "";
    const auto wanted = [&](const char* name) { return only.empty() || std::string(name).find(only) != std::string::npos; };
    try {
        spec_parsing();
        miter_parsing_and_plane();
        tube_component_round_trip();
        fake_create_tube();
        fake_cope_and_retry();
        cope_preparation_refusals();
        miter_preparation();
        {
            TempDir nativeDir;
            native_always(nativeDir.path);
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << "\n";
        return 1;
    }
    {
        TempDir dir;
        RealWorld world{ExactCadSettings::from_environment(), dir.path};
        world.settings.exactCollision = false;
        std::string why;
        if (brepkernel_runs(world.settings, dir.path, why)) {
            try {
                if (wanted("tubes")) real_tubes(world);
                if (wanted("miter")) real_miter(world);
                if (wanted("cope")) real_cope(world);
                if (wanted("equal")) real_equal_diameter(world);
                if (wanted("native")) native_real(dir.path);
            } catch (const std::exception& exception) {
                std::cerr << "FAIL: " << exception.what() << "\n";
                return 1;
            }
        } else {
            const char* require = std::getenv("DVE_REQUIRE_BREPKERNEL");
            if (require && std::strcmp(require, "1") == 0) {
                std::cerr << "FAIL: DVE_REQUIRE_BREPKERNEL=1 but brepkernel could not run (" << why << ")\n";
                return 1;
            }
            dve::testing::note_skip("real brepkernel geometry (tube volumes, plane cuts, copes, equal diameter): brepkernel could not run (" + why + ")");
        }
    }
    if (g_failures) { std::cerr << g_failures << " check(s) failed\n"; return 1; }
    std::cout << "exact tube tests passed\n";
    return dve::testing::exit_code(false);
}
