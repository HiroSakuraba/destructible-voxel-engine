#pragma once

// Hollow tubes, coping cuts and plane (miter) cuts as exact B-rep objects.
//
// The CAD Booleans of editor_exact_cad.hpp already give certified results through brepkernel.
// This adds three uses of them that tube fabrication needs:
//
//   create_exact_tube   a hollow round, square or rectangular tube, built as the outer solid minus
//                       a longer bore (brepkernel `primitive` + certified difference). The object is
//                       centred on its mesh-local origin, its axis along X, Y or Z, and carries a
//                       `dve.exact_tube` record (profile, outer size, wall, length, axis, cuts).
//   prepare/run_exact_cope  "cope to": the solid outer body of a mating tube B (grown by an
//                       optional fit-up gap) is subtracted from tube A, so the saddle cut face lies
//                       on B's true surface through both of A's walls.
//   prepare/run_exact_miter a plane cut: a large slab on one side of a plane at a given angle is
//                       subtracted from the tube, so the cut face lies exactly on the plane through
//                       both walls; the chosen side is kept.
//
// Nothing here is approximate: each cut is one certified brepkernel Boolean, or it is refused with
// brepkernel's reason and nothing changes. The single exception is documented: two outer sizes
// that touch tangentially (equal diameters at 90 degrees) are refused by brepkernel at zero gap, so
// the cope can be repeated once with a tiny clearance (ExactCopeOptions::tangentClearanceMeters,
// 0.01 mm) and the result says so; it is certified for that stated geometry, never for the zero gap.

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "dve/editor_exact_cad.hpp"

namespace dve::editor {

inline constexpr std::string_view kExactTubeComponent = "dve.exact_tube";

enum class TubeProfile { Round, Square, Rectangular };

// All lengths in metres in the tube's mesh-local frame (before voxelScale). The tube is centred on
// the origin; `axis` is x, y or z. `width` is the outer diameter (round) or the outer size across
// the first transverse axis, `height` across the second one (equal to `width` for round and
// square). The transverse axes are the two other axes in ascending order: x -> (y, z),
// y -> (x, z), z -> (x, y).
struct TubeSpec {
    TubeProfile profile{TubeProfile::Round};
    double width{0.0508};
    double height{0.0508};
    double wall{0.003};
    double length{0.6};
    char axis{'y'};
};

[[nodiscard]] std::string validate_tube_spec(const TubeSpec& spec);   // "" when valid
struct TubeSpecParse {
    std::optional<TubeSpec> spec;
    std::string error;
};
// "round 50.8 3 600 y", "square 40 2 500 x", "rect 80x40 3 600 z": outer size(s), wall, length in
// millimetres (suffix mm, cm, m or in on any number), optional axis letter last (default y).
[[nodiscard]] TubeSpecParse parse_tube_spec(std::string_view text);
[[nodiscard]] std::string format_tube_spec(const TubeSpec& spec);      // the text above, in mm
[[nodiscard]] std::string describe_tube_spec(const TubeSpec& spec);    // "Round tube Ø50.8 x 3 mm wall, 600 mm long, along Y"
[[nodiscard]] double tube_cross_section_area(const TubeSpec& spec);    // material area, m2
[[nodiscard]] double tube_volume(const TubeSpec& spec);                // analytic, m3
[[nodiscard]] std::array<int, 3> tube_axes(char axis);                 // {axis, first transverse, second transverse}; {-1,-1,-1} if invalid

struct ExactTubeRecord {
    TubeSpec spec;
    std::string cuts;      // "; "-separated record of the cope and plane cuts applied, "" for the stock tube
};
[[nodiscard]] Component make_exact_tube_component(const TubeSpec& spec, const std::string& cuts = {});
[[nodiscard]] std::optional<ExactTubeRecord> exact_tube_record(const EditorObject& object);

struct ExactTubeResult {
    CookedPolygonAsset mesh;
    ExactBrepSource source;
    ExactCollisionResult collision;
    TubeSpec spec;
    double exactVolume{};     // certified, m3 (mesh frame)
    std::string summary;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};
[[nodiscard]] ExactTubeResult create_exact_tube(
    const std::filesystem::path& projectRoot, const TubeSpec& spec, EditorObjectId id,
    const ExactCadSettings& settings = ExactCadSettings::from_environment(), const ExactCadRunner& runner = {});

// One cope or plane cut, prepared on the thread that owns the document and run anywhere.
struct ExactTubeCutRequest {
    ExactBooleanRequest boolean;       // difference: A minus the cutter
    std::string kind;                  // "cope" or "miter"
    std::string record;                // what is appended to the tube's cut record
    double gapMeters{};                // cope: fit-up gap in B's mesh frame
    bool tangentRetry{};               // cope: repeat once with tangentClearanceMeters when brepkernel refuses a near-tangent pair
    double tangentClearanceMeters{};
    std::optional<ExactTubeRecord> tubeA;   // A's record, carried to the result
    std::optional<TubeSpec> cutterTube;     // cope: B's tube, the cutter is its outer body grown by the gap
    std::string target;                // cope: name of B, for the record
    double cutterScale{1.0};           // cope: B's uniform scale (mesh metres -> world metres), for messages
    std::string error;                 // non-empty: cannot run
};
struct ExactTubeCutResult {
    ExactBooleanResult boolean;
    std::string record;                // the cut record to store (includes any clearance that was applied)
    bool clearanceApplied{};
    double appliedGapMeters{};
    std::string firstRefusal;          // brepkernel's refusal of the zero-gap attempt when the clearance was applied
    std::string warning;               // e.g. the cut left several pieces
    [[nodiscard]] explicit operator bool() const noexcept { return bool(boolean); }
};

struct ExactCopeOptions {
    double gapWorldMeters{0.0};                  // fit-up gap, world metres (default 0)
    double tangentClearanceMeters{1.0e-5};       // clearance used once when the zero-gap cut is refused (0.01 mm)
    bool retryTangent{true};
};
[[nodiscard]] ExactTubeCutRequest prepare_exact_cope(
    const std::filesystem::path& projectRoot, const EditorDocument& document, EditorObjectId a, EditorObjectId b,
    const ExactCopeOptions& options = {});

// The largest fit-up gap a cope accepts, in metres (100 mm).
inline constexpr double kMaxFitUpGapMeters = 0.1;

// The typed fit-up gap: a length in millimetres by default (units mm, cm, m, in; scientific notation
// such as 1e-3 is a plain number, so 0.001 mm), never negative.
struct ExactGapParse {
    std::optional<double> meters;
    std::string error;
};
[[nodiscard]] ExactGapParse parse_fit_up_gap(std::string_view text);

enum class MiterKeep { Low, High };              // keep the end on the -axis side (Low) or +axis side (High) of the plane
struct ExactMiterSpec {
    double angleDegrees{45.0};                   // from a square cut: 0 = square, 45 = a 90 degree joint, |angle| < 85
    double positionWorldMeters{0.0};             // where the plane crosses the tube's axis, from its centre, world metres
    char tilt{'u'};                              // 'u' or 'v': which transverse direction the plane leans toward
    MiterKeep keep{MiterKeep::Low};
};
[[nodiscard]] std::string validate_miter_spec(const ExactMiterSpec& spec);
// "47.5", "47.5 high", "30 v low at 120" : angle, optional side, tilt direction and axial position (mm).
struct ExactMiterParse {
    std::optional<ExactMiterSpec> spec;
    std::string error;
};
[[nodiscard]] ExactMiterParse parse_miter_spec(std::string_view text);
[[nodiscard]] std::string format_miter_spec(const ExactMiterSpec& spec);
// Plane through the tube axis point `position` along its axis, leaning by `angle`; `normal` points to the removed side.
struct MiterPlane {
    std::array<double, 3> point{}, normal{};
};
[[nodiscard]] MiterPlane miter_plane(const TubeSpec& spec, const ExactMiterSpec& miter, double positionMeshMeters);
[[nodiscard]] ExactTubeCutRequest prepare_exact_miter(
    const std::filesystem::path& projectRoot, const EditorDocument& document, EditorObjectId a, const ExactMiterSpec& miter);

[[nodiscard]] ExactTubeCutResult run_exact_tube_cut(
    const std::filesystem::path& projectRoot, const ExactTubeCutRequest& request,
    const ExactCadSettings& settings = ExactCadSettings::from_environment(), const ExactCadRunner& runner = {});
// The cut record of a result: `before` (A's cuts) and one more entry.
[[nodiscard]] std::string append_tube_cut(const std::string& before, const std::string& cut);

} // namespace dve::editor
