#pragma once

// Exact CAD (B-rep) source for polygon objects, through the brepkernel command line tool.
//
// A polygon object can carry an exact B-rep next to its triangles: the `dve.exact_brep`
// component names a project-owned `.brep` file (sources/exact/, content-addressed) in the
// object's mesh-local frame (meters, +Y up, before voxelScale). Two operations use it:
//
//   import_exact_cad               STEP / BREP / OpenUSD B-rep -> project .brep + polygon mesh
//   exact_boolean_editor_polygons  certified Boolean of two such objects, re-meshed from the
//                                  exact result (curves stay curves at any detail level)
//
// Both run brepkernel as an external process (argv, no shell) and read its JSON report.
// brepkernel either certifies a result or refuses: a refusal is reported with its reason and
// the uncertified second opinions, and nothing in the scene changes.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/component.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/polygon_asset.hpp"

namespace dve::editor {

inline constexpr std::string_view kExactBrepComponent = "dve.exact_brep";

struct ExactCadSettings {
    // argv prefix that runs brepkernel. Default: $DVE_BREPKERNEL split on spaces, else
    // `brepkernel` when found on PATH, else `python3 -m brepkernel`.
    std::vector<std::string> command;
    double deflectionMeters{0.0};      // chord deflection in the mesh frame; 0 = brepkernel default
    double angularRadians{0.35};
    bool advisory{true};               // attach second opinions when a Boolean is refused
    std::chrono::seconds timeout{300};
    std::uint64_t maximumOutputBytes{1U << 20U};
    // Exact convex collision (dve.collision_convex_set) built with each import and exact
    // Boolean, for extruded parts; other parts keep the single hull or bounding box.
    bool exactCollision{true};
    double collisionMaxError{1e-4};    // polygon error for arcs, meters (pieces stay inside the part)

    [[nodiscard]] static ExactCadSettings from_environment();
};

struct ExactCadProcessResult {
    bool launched{};
    bool timedOut{};
    int exitCode{-1};
    std::string output;                // merged stdout/stderr, bounded
    std::string error;
};

// Runs argv[0] (PATH lookup) with argv; no shell is involved, so paths need no quoting.
using ExactCadRunner = std::function<ExactCadProcessResult(
    const std::vector<std::string>& argv, std::chrono::seconds timeout, std::uint64_t maximumOutputBytes)>;
[[nodiscard]] ExactCadProcessResult run_exact_cad_process(
    const std::vector<std::string>& argv, std::chrono::seconds timeout, std::uint64_t maximumOutputBytes);

struct ExactBrepSource {
    std::filesystem::path relativePath;   // project-relative, generic separators
    std::string sha256;
    std::string origin;                   // where it came from (CAD file or Boolean), informational
    // How finely the object's triangles follow the exact surface: brepkernel's chord deflection
    // in the mesh-local frame (metres, before voxelScale) and angular deflection. 0 = not recorded
    // (meshes from before this was stored used brepkernel's default, 1/500 of the part's size).
    double deflectionMeters{0.0};
    double angularRadians{0.0};
};
[[nodiscard]] std::optional<ExactBrepSource> exact_brep_source(const EditorObject& object);
[[nodiscard]] Component make_exact_brep_component(const ExactBrepSource& source);

// 3x4 row-major affine map from `b`'s mesh-local frame into `a`'s (pose and voxelScale of both,
// attachments included), computed in double precision from the authored transforms.
[[nodiscard]] std::optional<std::array<double, 12>> exact_relative_placement(
    const EditorDocument& document, EditorObjectId a, EditorObjectId b);

// Certified convex pieces for an exact source, as a dve.collision_convex_set component.
struct ExactCollisionResult {
    std::optional<Component> component;   // id unset: the caller assigns one
    std::string note;                      // what happened, for the status line
    bool refused{};                        // brepkernel refused (not an extrusion, ...)
};
[[nodiscard]] ExactCollisionResult exact_convex_collision(
    const std::filesystem::path& projectRoot, const ExactBrepSource& source,
    const ExactCadSettings& settings = ExactCadSettings::from_environment(), const ExactCadRunner& runner = {});

struct ExactCadImportResult {
    CookedPolygonAsset mesh;
    ExactBrepSource source;
    ExactCollisionResult collision;
    std::string summary;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
[[nodiscard]] ExactCadImportResult import_exact_cad(
    const std::filesystem::path& projectRoot, const std::filesystem::path& cadFile, EditorObjectId id,
    const ExactCadSettings& settings = ExactCadSettings::from_environment(), const ExactCadRunner& runner = {});

struct ExactBooleanResult {
    PolygonEditResult edit;            // mesh in A's mesh-local frame, A's materials
    ExactBrepSource source;            // project-owned .brep of the certified result
    ExactCollisionResult collision;    // convex pieces of the result, when it is an extrusion
    bool refused{};
    bool empty{};                      // certified, but no material left
    std::string refusalKind;
    std::string advisory;              // one line of uncertified second opinions, when refused
    std::string summary;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(edit); }
};
// Background-safe split: prepare on the thread that owns the document (it copies everything
// the tool needs), run anywhere (no document access), then apply on the owning thread after
// checking `operandStamp` still matches.
struct ExactBooleanRequest {
    EditorObjectId a{}, b{};
    MeshBoolean operation{MeshBoolean::Union};
    std::filesystem::path brepA, brepB;           // absolute
    std::array<double, 12> placement{};           // B mesh-local -> A mesh-local
    std::string nameA;
    CookedPolygonAsset materialsFromA;            // materials/bindings/textures only
    std::uint64_t operandStamp{};
    double worldVolumeScale{1.0};                 // |sA.x sA.y sA.z|: A mesh-frame volume -> world
    std::string error;                            // non-empty: cannot run
};
[[nodiscard]] std::uint64_t exact_operand_stamp(const EditorDocument& document, EditorObjectId a, EditorObjectId b);
[[nodiscard]] ExactBooleanRequest prepare_exact_boolean(
    const std::filesystem::path& projectRoot, const EditorDocument& document, EditorObjectId a, EditorObjectId b,
    MeshBoolean operation);
[[nodiscard]] ExactBooleanResult run_exact_boolean(
    const std::filesystem::path& projectRoot, const ExactBooleanRequest& request,
    const ExactCadSettings& settings = ExactCadSettings::from_environment(), const ExactCadRunner& runner = {});
// prepare + run in one call (tests, scripting).
[[nodiscard]] ExactBooleanResult exact_boolean_editor_polygons(
    const std::filesystem::path& projectRoot, const EditorDocument& document, EditorObjectId a, EditorObjectId b,
    MeshBoolean operation, const ExactCadSettings& settings = ExactCadSettings::from_environment(),
    const ExactCadRunner& runner = {});

[[nodiscard]] bool is_exact_cad_source(const std::filesystem::path& path);

// Re-mesh an exact source at a chosen accuracy (CAD Detail): brepkernel `mesh` with the given chord
// deflection (mesh-local metres; 0 = brepkernel's default). The source itself does not change.
struct ExactCadRemeshResult {
    CookedPolygonAsset mesh;
    double deflectionMeters{};   // what brepkernel actually used
    double angularRadians{};
    std::uint64_t triangles{};
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
[[nodiscard]] ExactCadRemeshResult remesh_exact_cad(
    const std::filesystem::path& projectRoot, const ExactBrepSource& source, EditorObjectId id, double deflectionMeters,
    const ExactCadSettings& settings = ExactCadSettings::from_environment(), const ExactCadRunner& runner = {});
// Largest distance, in world metres, between the triangles and the exact surface for this object
// (the recorded deflection times its largest axis scale); nullopt when not recorded.
[[nodiscard]] std::optional<double> exact_mesh_world_error(const EditorObject& object);

// Exact answers for measuring and snapping (brepkernel `describe` / `query`), in the mesh-local
// frame (metres, +Y up, before voxelScale). `unsupported` is set when the installed brepkernel
// predates these commands, so the editor can say "update brepkernel" instead of a raw error.
using ExactPoint = std::array<double, 3>;
struct ExactCadFace {
    int index{-1};                 // mesh / describe order
    std::string type;              // plane, cylinder, cone, sphere, torus, bspline, bezier, other
    double area{};
    ExactPoint origin{}, normal{};                 // plane
    ExactPoint axisOrigin{}, axis{}, centre{};     // cylinder / cone / sphere / torus
    double radius{}, majorRadius{}, minorRadius{}, halfAngle{};
    bool hole{};                   // cylinder whose material is outside (a bore)
};
struct ExactCadEdge {
    std::string type;              // line, circle, curve
    ExactPoint start{}, end{}, mid{};
    double length{};
};
struct ExactCadCircle {
    ExactPoint centre{}, axis{};
    double radius{};
    bool full{};
};
struct ExactCadDescription {
    std::vector<ExactCadFace> faces;
    std::vector<ExactPoint> vertices;
    std::vector<ExactCadEdge> edges;
    std::vector<ExactCadCircle> circles;
    double volume{}, area{};
    std::string error;
    bool unsupported{};
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
[[nodiscard]] ExactCadDescription describe_exact_cad(
    const std::filesystem::path& projectRoot, const ExactBrepSource& source,
    const ExactCadSettings& settings = ExactCadSettings::from_environment(), const ExactCadRunner& runner = {});

struct ExactCadQueryHit {
    bool found{};
    ExactPoint closest{}, normal{};
    double distance{};
    int face{-1};
};
struct ExactCadQueryResult {
    std::vector<ExactCadQueryHit> hits;   // one per point, same order
    std::string error;
    bool unsupported{};
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
[[nodiscard]] ExactCadQueryResult query_exact_cad(
    const std::filesystem::path& projectRoot, const ExactBrepSource& source, const std::vector<ExactPoint>& points,
    const ExactCadSettings& settings = ExactCadSettings::from_environment(), const ExactCadRunner& runner = {});

// "Hole Ø 12.000 mm", "Cylinder Ø 6.000 mm", "Plane", "Sphere Ø 20.000 mm", ... in millimetres;
// `scale` multiplies lengths (an object's uniform world scale; 1 in the mesh frame).
[[nodiscard]] std::string describe_exact_face(const ExactCadFace& face, double scale = 1.0);

} // namespace dve::editor
