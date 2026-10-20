#pragma once
#include "dve/nurbs.hpp"
#include <span>
namespace dve {
// Ordered sections, degree 1 in V. Curves must share degree; normalized knot
// domains are refined to their union. No degree elevation or seam inference.
[[nodiscard]] std::optional<NurbsSource> loft_nurbs(std::span<const NurbsSource> profiles,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsSource> reverse_nurbs_curve(const NurbsSource&,std::string* error=nullptr);
// Exact cyclic reindexing for closed, uniform, piecewise-Bezier curves only.
[[nodiscard]] std::optional<NurbsSource> shift_nurbs_seam(const NurbsSource&,std::string* error=nullptr);
struct NurbsSweepOptions {
    unsigned sections{16}; // 2..64; uniform rail parameter, not arc length
    double twistRadians{};
    double endScale{1}; // positive; start scale is 1
    std::array<double,3> profileOrigin{};
};
// Sampled parallel-transport sections joined by a ruled loft. This approximates
// the rail between stations; it is not an exact or error-certified pipe.
// Section controls must lie in the plane through profileOrigin perpendicular
// to the initial rail tangent. Rail placement supplies the section origin.
// Closed rails, stationary samples and tangent reversals are refused.
[[nodiscard]] std::optional<NurbsSource> sweep_nurbs(const NurbsSource& profile,const NurbsSource& rail,const NurbsSweepOptions& options={},std::string* error=nullptr);
}
