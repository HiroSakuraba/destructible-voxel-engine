#pragma once
#include "dve/nurbs.hpp"
namespace dve {
struct NurbsDisplayGrid { std::vector<double> u,v; };
[[nodiscard]] bool validate_nurbs_display_grid(const NurbsSource&,const NurbsDisplayGrid&,std::string* error=nullptr);
[[nodiscard]] std::optional<NurbsDisplayGrid> nurbs_display_grid(const NurbsSource&,const NurbsDisplayGrid* seed=nullptr,std::string* error=nullptr,std::stop_token stop={});
// Explicit tensor grid with one-sided corner derivatives and bounded work.
[[nodiscard]] std::optional<CookedPolygonAsset> tessellate_nurbs_grid(const NurbsSource&,std::uint64_t,const NurbsDisplayGrid&,std::string* error=nullptr,std::stop_token stop={});
}
