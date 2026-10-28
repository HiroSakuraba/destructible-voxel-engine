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

// G2 bridge between two curves (Wittens, "Curvature Beziers"). A cubic Bezier from the nearest
// end of A to the nearest end of B that continues each curve's tangent direction and curvature
// (signed, so S-bends work). With P1 = A + l0*T0 and P2 = D - l1*T1, signed curvature at the ends
// gives the coupled system
//     1.5 k0 l0^2 + s l1 - c0 = 0,    1.5 k1 l1^2 + s l0 - c1 = 0,
//     s = (T0 x T1).n,  c0 = (T0 x (D-A)).n,  c1 = ((D-A) x T1).n
// whose positive solutions are found from the quartic in l0 (or directly when s = 0). When
// several solutions exist they are different curves; the one kept is the article's most stable,
// least loopy choice: weight (l0 l1 acos(-cos theta) / |l|^2)^2, theta between the gradients of
// the two equations (near-opposite gradients mean two solutions about to merge and vanish).
// The ends, their tangents and curvature vectors must lie in one plane (a planar cubic has one
// osculating plane at each end). Some configurations have no cubic solution (for example
// parallel straight ends offset sideways); those are refused.
struct NurbsCurveBridge {
    NurbsSource source;           // degree-3 Bezier, 4 controls
    bool endOfA{}, endOfB{};      // which ends were bridged (true: upper end of the domain)
    double handleA{}, handleB{};  // l0, l1
    unsigned solutions{};         // positive exact solutions found
    // Set when no curvature-matched cubic exists and tangentFallback was allowed: the bridge
    // then only continues the tangents (G1), with handles a third of the chord, and
    // fallbackReason says why G2 was impossible.
    bool tangentOnly{};
    std::string fallbackReason;
};
[[nodiscard]] std::optional<NurbsCurveBridge> bridge_nurbs_curves_g2(
    const NurbsSource& a, const NurbsSource& b, std::string* error = nullptr,
    bool tangentFallback = false);

} // namespace dve
