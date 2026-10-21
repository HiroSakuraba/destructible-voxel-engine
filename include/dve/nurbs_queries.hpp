#pragma once
#include "dve/nurbs.hpp"
namespace dve {
struct NurbsLength {
    double length{}, estimatedError{};
    unsigned evaluations{};
};
// Integrates analytic speed over [begin,end], splitting at every knot span.
// Tolerance is absolute in the supplied metric. Error is a numerical estimate,
// not a certified bound. Refuses exhausted work/depth budgets without a result.
// Scale applies before measuring; negative and zero components are supported.
[[nodiscard]] std::optional<NurbsLength> measure_nurbs_curve(
    const NurbsSource&, double begin, double end, double tolerance=1e-7,
    std::array<double,3> scale={1,1,1}, std::string* error=nullptr);
}
