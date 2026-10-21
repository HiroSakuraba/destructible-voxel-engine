#pragma once
#include "dve/nurbs_construction.hpp"
#include "dve/polygon_asset.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
namespace dve {
// Natural cubic interpolation of compatible sections in homogeneous coordinates.
// Interpolates profiles at V=0,1,...; refuses nonpositive spline weights.
[[nodiscard]] std::optional<NurbsSource> smooth_loft_nurbs(std::span<const NurbsSource>,std::string* error=nullptr);
struct NurbsFit { NurbsSource source; double maxSampledError{}; };
// Uniform sample parameters; polynomial least squares, fixed endpoints, 2..64 CVs.
// Error is measured at samples, not a certified whole-curve bound.
[[nodiscard]] std::optional<NurbsFit> fit_nurbs_curve(std::span<const std::array<double,3>>,unsigned controls,double tolerance,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsFit> rebuild_nurbs_curve(const NurbsSource&,unsigned controls,double tolerance,std::string* error=nullptr);
// Match moving U-min to reference U-max. V axes must match exactly. C1 matches
// homogeneous first derivatives; these are separate patches, not topological sewing.
[[nodiscard]] std::optional<NurbsSource> attach_nurbs_u(const NurbsSource& reference,const NurbsSource& moving,bool c1,std::string* error=nullptr);
struct NurbsProjection { double u{},v{},distance{}; std::array<double,3> position{}; };
// Multi-seed, constrained numerical nearest-point search; no global minimum certificate.
[[nodiscard]] std::optional<NurbsProjection> project_nurbs_point(const NurbsSource& surface,std::array<double,3> point,double tolerance=1e-6,std::string* error=nullptr);
// Polyline guide through projected samples; connecting segments need not lie on surface.
[[nodiscard]] std::optional<NurbsSource> project_nurbs_curve(const NurbsSource& curve,const NurbsSource& surface,unsigned samples=33,std::string* error=nullptr);
// Exact affine-patch intersection segment only; refuses curved, parallel and coplanar cases.
[[nodiscard]] std::optional<NurbsSource> intersect_nurbs_planes(const NurbsSource&,const NurbsSource&,double tolerance=1e-8,std::string* error=nullptr);
// Exact rectangular parameter crop using source splits; no arbitrary trim loops/holes.
[[nodiscard]] std::optional<NurbsSource> crop_nurbs_surface(const NurbsSource&,double u0,double u1,double v0,double v1,std::string* error=nullptr);
// Exact rational circular fillet arc for a three-control degree-one corner.
// Returns the arc alone; the two incident legs remain in the original source.
[[nodiscard]] std::optional<NurbsSource> fillet_nurbs_corner(const NurbsSource&,double radius,std::string* error=nullptr);
// Crack-free tensor grid within one patch, refined by sampled chord/normal tests.
// Duplicated cell corners preserve knot-side normals. No certified error bound
// or cross-object edge synchronization. Bounded evaluations and 65536 vertices.
[[nodiscard]] std::optional<CookedPolygonAsset> tessellate_nurbs_adaptive(const NurbsSource&,std::uint64_t id,double chordTolerance=.01,double normalAngle=.15,std::string* error=nullptr);
}
