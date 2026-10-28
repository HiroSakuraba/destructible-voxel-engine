#pragma once
#include "dve/nurbs.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

// Curvature and geometric continuity for NURBS curves and surfaces.
//
// Tangent alignment (G1) is not smoothness: a join can share a tangent and still show a kink
// in highlights, because curvature jumps there. These queries measure curvature directly, so
// the editor can draw a curvature comb and report G0/G1/G2 at knots, joins and seams.
// All measurements are numerical (floating-point evaluation); none is a certified bound.
namespace dve {

// Position and derivatives up to second order. For a curve, dv/duv/dvv are zero.
struct NurbsSecondSample {
    std::array<double,3> position{}, du{}, dv{}, duu{}, duv{}, dvv{};
};
// fromLeft* evaluates the left-hand limit at an interior knot (the default is the right side;
// the upper end of the domain always uses its left side).
[[nodiscard]] std::optional<NurbsSecondSample> evaluate_nurbs_second(
    const NurbsSource&, double u, double v = 0, bool fromLeftU = false, bool fromLeftV = false,
    std::string* error = nullptr);

struct NurbsCurvePoint {
    double u{};
    std::array<double,3> position{}, tangent{}; // unit tangent
    // Points from the curve towards the centre of curvature; its length is the curvature.
    std::array<double,3> curvatureVector{};
    double curvature{};                          // 1 / radius; 0 on straight parts
};
[[nodiscard]] std::optional<NurbsCurvePoint> nurbs_curve_curvature(
    const NurbsSource& curve, double u, bool fromLeft = false, std::string* error = nullptr);

// Samples for drawing a curvature comb: samplesPerSpan per nonempty knot span, the domain
// ends, and both sides of every interior knot (so a jump shows as two teeth at one place).
struct NurbsCurvatureComb {
    std::vector<NurbsCurvePoint> samples;
    double maximumCurvature{};
};
[[nodiscard]] std::optional<NurbsCurvatureComb> nurbs_curvature_comb(
    const NurbsSource& curve, unsigned samplesPerSpan = 16, std::string* error = nullptr);

enum class NurbsContinuity { None, G0, G1, G2 };
[[nodiscard]] const char* nurbs_continuity_name(NurbsContinuity) noexcept;

struct NurbsContinuityTolerance {
    double position{1e-6};        // relative to the size of the geometry (bounding diagonal)
    double angleRadians{1e-4};
    double curvature{1e-3};       // relative to the larger curvature, plus this / size
};

// One place where continuity was measured.
struct NurbsContinuityBreak {
    double parameter{};           // curve: knot value; join/seam: boundary fraction
    double gap{}, angleRadians{}, curvatureA{}, curvatureB{}, curvatureDifference{};
    NurbsContinuity level{NurbsContinuity::G2};
};
struct NurbsContinuityReport {
    NurbsContinuity level{NurbsContinuity::G2}; // worst over all measured places
    double maximumGap{}, maximumAngleRadians{}, maximumCurvatureDifference{};
    double maximumCurvature{};
    std::vector<NurbsContinuityBreak> places;   // every measured place
    std::string description;                    // one line for the status bar
};

// Interior knots of one curve. Knots of multiplicity below degree-1 are G2 by construction
// but are still measured.
[[nodiscard]] std::optional<NurbsContinuityReport> nurbs_curve_internal_continuity(
    const NurbsSource& curve, NurbsContinuityTolerance = {}, std::string* error = nullptr);

// Two curves in one frame: the closest pair of ends is measured, with B continuing A.
struct NurbsCurveJoinReport : NurbsContinuityReport {
    bool endOfA{}, endOfB{};      // which ends met (true: the upper end of the domain)
};
[[nodiscard]] std::optional<NurbsCurveJoinReport> nurbs_curve_join_continuity(
    const NurbsSource& a, const NurbsSource& b, NurbsContinuityTolerance = {},
    std::string* error = nullptr);

// Two surfaces in one frame: the closest pair of boundaries (either direction) is measured at
// `samples` places along it. G1 compares normals; G2 compares normal curvature across the
// boundary, in the direction perpendicular to it.
struct NurbsSurfaceJoinReport : NurbsContinuityReport {
    unsigned boundaryA{}, boundaryB{}; // NurbsBoundary values
    bool reversed{};
};
[[nodiscard]] std::optional<NurbsSurfaceJoinReport> nurbs_surface_join_continuity(
    const NurbsSource& a, const NurbsSource& b, unsigned samples = 9,
    NurbsContinuityTolerance = {}, std::string* error = nullptr);

} // namespace dve
