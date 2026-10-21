#pragma once
#include "dve/nurbs.hpp"
namespace dve::detail {
// Internal hot-loop entry: caller validates the immutable source once and
// supplies its positive maximum weight. Parameter/overflow checks remain.
[[nodiscard]] std::optional<NurbsSample> evaluate_validated_nurbs(
    const NurbsSource&,double u,double v,double weightScale,std::string* error=nullptr);
}
