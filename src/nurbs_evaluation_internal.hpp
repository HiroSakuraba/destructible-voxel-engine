#pragma once
#include "dve/nurbs.hpp"
#include <array>
namespace dve::detail {
// Internal hot-loop entry: caller validates the immutable source once and
// supplies its positive maximum weight. Parameter/overflow checks remain.
[[nodiscard]] std::optional<NurbsSample> evaluate_validated_nurbs(
    const NurbsSource&,double u,double v,double weightScale,std::string* error=nullptr);
// Homogeneous (x*w,y*w,z*w,w) third cross-boundary control of a moving patch whose start
// (U minimum) is to match the second derivative of a reference patch's end (U maximum), given
// the reference's last three homogeneous controls in that row (r3 = third from the end) and the
// moving row's already-matched first two. Unnormalized U domains; second derivatives are
// matched in those parameters, so with first derivatives matched the join is C2 (and G2).
[[nodiscard]] std::array<double,4> second_row_control(
    const NurbsAxis& referenceU,unsigned referenceColumns,const NurbsAxis& movingU,
    std::array<double,4> r3,std::array<double,4> r2,std::array<double,4> r1,
    std::array<double,4> q0,std::array<double,4> q1);
}
