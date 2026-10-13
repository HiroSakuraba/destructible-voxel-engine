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
};
[[nodiscard]] std::optional<ExactBrepSource> exact_brep_source(const EditorObject& object);
[[nodiscard]] Component make_exact_brep_component(const ExactBrepSource& source);

// 3x4 row-major affine map from `b`'s mesh-local frame into `a`'s (pose and voxelScale of both,
// attachments included), computed in double precision from the authored transforms.
[[nodiscard]] std::optional<std::array<double, 12>> exact_relative_placement(
    const EditorDocument& document, EditorObjectId a, EditorObjectId b);

struct ExactCadImportResult {
    CookedPolygonAsset mesh;
    ExactBrepSource source;
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

} // namespace dve::editor
