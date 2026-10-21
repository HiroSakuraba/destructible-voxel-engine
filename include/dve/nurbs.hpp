#pragma once
#include "dve/polygon_asset.hpp"
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dve {
struct NurbsPoint {
    double x{}, y{}, z{}, weight{1};
    bool operator==(const NurbsPoint&) const = default;
};
struct NurbsAxis {
    unsigned degree{3};
    std::vector<double> knots;
    bool operator==(const NurbsAxis&) const = default;
};
// Full expanded knot convention. Controls are [v * columns + u]. This first
// release supports clamped open axes (including geometrically closed seams).
// Curve sources have rows=1 and v={0,{0,1}}; no periodic shortcut is inferred.
struct NurbsSource {
    unsigned columns{}, rows{1};
    NurbsAxis u, v{0,{0,1}};
    std::vector<NurbsPoint> controls;
    unsigned samplesPerSpan{8};
    // Display metadata; v1 sources decode with legacy shared crease vertices.
    bool creaseNormals{true};
    std::array<double,2> textureU{0,1},textureV{0,1};
    // v3 display policy. Surface tolerances use source-local units/radians;
    // adaptive subdivision uses sampled tests, not certified interval bounds.
    bool adaptiveDisplay{};
    double chordTolerance{.01},normalAngle{.15};
    bool operator==(const NurbsSource&) const = default;
    [[nodiscard]] bool is_curve() const noexcept { return rows==1; }
};
struct NurbsSample {
    std::array<double,3> position{}, du{}, dv{};
};
[[nodiscard]] bool validate_nurbs(const NurbsSource&,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsSample> evaluate_nurbs(const NurbsSource&,double u,double v=0,std::string* error=nullptr);
[[nodiscard]] NurbsSource make_nurbs_patch(double size=4);
[[nodiscard]] NurbsSource make_nurbs_arc(double radius=1,double angleRadians=6.283185307179586);
[[nodiscard]] std::optional<NurbsSource> extrude_nurbs(const NurbsSource& curve,std::array<double,3> offset,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsSource> revolve_nurbs(const NurbsSource& curve,double angleRadians=6.283185307179586,std::string* error=nullptr);
// Shape-preserving refinement in homogeneous coordinates (floating-point arithmetic).
[[nodiscard]] std::optional<NurbsSource> insert_nurbs_knot(const NurbsSource&,bool alongV,double parameter,std::string* error=nullptr);
[[nodiscard]] std::string encode_nurbs(const NurbsSource&);
[[nodiscard]] std::optional<NurbsSource> decode_nurbs(std::string_view,std::string* error=nullptr);
// Uses the persisted fixed/adaptive display policy; neither is error-certified.
// Curves use a narrow two-sided guide ribbon (no implied thickness/volume).
[[nodiscard]] std::optional<CookedPolygonAsset> tessellate_nurbs(const NurbsSource&,std::uint64_t objectId,std::string* error=nullptr);
}

