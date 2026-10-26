#pragma once
#include "dve/nurbs_tessellation.hpp"
#include <cstdint>
namespace dve {
using NurbsUV=std::array<double,2>;
// Normalized UV polylines. Closure is implicit; do not repeat the first point.
struct NurbsTrimLoop {std::uint64_t id{};std::vector<NurbsUV> points;bool operator==(const NurbsTrimLoop&)const=default;};
struct NurbsTrimRegion {
    std::vector<NurbsTrimLoop> loops;
    // Empty selects odd parity (outer regions with nested holes). Otherwise
    // select the containment regions containing these seeds, independent of winding.
    std::vector<NurbsUV> seeds;
    bool inverted{};
    bool operator==(const NurbsTrimRegion&)const=default;
};
[[nodiscard]] bool validate_nurbs_trim(const NurbsTrimRegion&,std::string* error=nullptr);
[[nodiscard]] bool nurbs_trim_contains(const NurbsTrimRegion&,NurbsUV);
// Add trim events to the display grid. Network callers must synchronize this
// schedule across seams before tessellation; no independent boundary splitting.
[[nodiscard]] std::optional<NurbsDisplayGrid> nurbs_trim_grid(const NurbsSource&,const NurbsDisplayGrid&,const NurbsTrimRegion&,std::string* error=nullptr);
[[nodiscard]] std::optional<CookedPolygonAsset> tessellate_nurbs_trimmed_grid(
    const NurbsSource&,std::uint64_t,const NurbsDisplayGrid&,const NurbsTrimRegion&,
    std::string* error=nullptr,std::stop_token={},std::vector<NurbsUV>* vertexParameters=nullptr);
}
