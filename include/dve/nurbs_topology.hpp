#pragma once
#include "dve/nurbs.hpp"
#include <utility>
namespace dve {
// Clamped tensor-product sources. Parameters use the existing axis domain.
// Splits retain that domain on each side and share the boundary controls.
// Homogeneous insertion preserves geometry within floating-point arithmetic.
[[nodiscard]] std::optional<std::pair<NurbsSource,NurbsSource>> split_nurbs(const NurbsSource&,bool alongV,double parameter,std::string* error=nullptr);
// Fix U when fixedV=false (returns a V curve); fix V otherwise (U curve).
// Endpoints are accepted. These are rational isocurves, not sampled polylines.
[[nodiscard]] std::optional<NurbsSource> extract_nurbs_isocurve(const NurbsSource&,bool fixedV,double parameter,std::string* error=nullptr);
// Parameter-rectangle loop: V-min, U-max, reversed V-max, reversed U-min.
// Includes coincident parameter edges; does not infer trims or unique topology.
[[nodiscard]] std::optional<std::array<NurbsSource,4>> extract_nurbs_boundaries(const NurbsSource&,std::string* error=nullptr);
// Raises one axis by one degree, through homogeneous Bezier decomposition.
// Preserves shape/parameter map but retains full-multiplicity span boundaries;
// it does not minimize knots. Supported output degree is at most 7.
[[nodiscard]] std::optional<NurbsSource> elevate_nurbs_degree(const NurbsSource&,bool alongV,std::string* error=nullptr);
// Reversal and transpose preserve positions while changing parameter direction.
// Reversing one axis or swapping U/V flips the oriented surface normal.
[[nodiscard]] std::optional<NurbsSource> reverse_nurbs_axis(const NurbsSource&,bool alongV,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsSource> transpose_nurbs(const NurbsSource&,std::string* error=nullptr);
}
