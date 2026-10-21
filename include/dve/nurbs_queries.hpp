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
struct NurbsLengthParameter {
    double parameter{}, lengthBefore{}, estimatedResidual{};
    NurbsLength total;
    unsigned evaluations{};
};
// Inverts a fraction of total curve length in the supplied metric. Uses a
// bracketed numerical solve; residual includes quadrature error estimates,
// not certified interval bounds. Zero metric length has no unique inverse.
// One shared 200,000-evaluation budget covers all integrations and iterations.
[[nodiscard]] std::optional<NurbsLengthParameter> nurbs_parameter_at_length_fraction(
    const NurbsSource&, double fraction, double tolerance=1e-7,
    std::array<double,3> scale={1,1,1}, std::string* error=nullptr);
}
