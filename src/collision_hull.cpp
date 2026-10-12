#include "dve/collision_hull.hpp"
#include "dve/voxel_metric.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
namespace dve {
bool validate_collision_hull_points(std::span<const Float3> points) noexcept {
    if(points.size()<4||points.size()>256)return false;
    Float3 minimum{INFINITY,INFINITY,INFINITY},maximum{-INFINITY,-INFINITY,-INFINITY};
    for(auto p:points){if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))return false;
        minimum={std::min(minimum.x,p.x),std::min(minimum.y,p.y),std::min(minimum.z,p.z)};maximum={std::max(maximum.x,p.x),std::max(maximum.y,p.y),std::max(maximum.z,p.z)};}
    double scale=length(subtract(maximum,minimum));if(!(scale>1e-8))return false;
    auto cross=[](Float3 a,Float3 b){return Float3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
    std::size_t second=0;double distance=0;for(std::size_t i=1;i<points.size();++i){auto d=length_squared(subtract(points[i],points[0]));if(d>distance){distance=d;second=i;}}
    Float3 normal{};double area=0;for(auto p:points){auto n=cross(subtract(points[second],points[0]),subtract(p,points[0]));double d=length_squared(n);if(d>area){area=d;normal=n;}}
    double volume6=0;for(auto p:points)volume6=std::max(volume6,std::abs(double(dot(normal,subtract(p,points[0])))));
    return volume6>scale*scale*scale*1e-9;
}
std::string encode_collision_hull_points(std::span<const Float3> points){std::ostringstream out;out.imbue(std::locale::classic());out<<"DVE_HULL 1 "<<points.size()<<'\n'<<std::setprecision(9);for(auto p:points)out<<p.x<<' '<<p.y<<' '<<p.z<<'\n';return out.str();}
std::optional<std::vector<Float3>> decode_collision_hull_points(std::string_view text){if(text.size()>32768)return {};std::istringstream in{std::string(text)};in.imbue(std::locale::classic());std::string header;unsigned version{};std::size_t count{};
    if(!(in>>header>>version>>count)||header!="DVE_HULL"||version!=1||count<4||count>256)return {};
    std::vector<Float3> points(count);for(auto& p:points)if(!(in>>p.x>>p.y>>p.z))return {};in>>std::ws;if(!in.eof()||!validate_collision_hull_points(points))return {};return points;}
std::optional<std::vector<Float3>> collision_hull_component_points(const Component& component){if(component.type!=kCollisionHullComponent)return {};auto it=component.properties.find("points");if(it==component.properties.end())return {};auto* value=std::get_if<std::string>(&it->second);return value?decode_collision_hull_points(*value):std::nullopt;}
std::vector<Float3> active_collision_hull_points(std::span<const Component> components){for(const auto& c:components)if(c.enabled&&c.type==kCollisionHullComponent){auto points=collision_hull_component_points(c);return points?*points:std::vector<Float3>{};}return {};}
Component bake_collision_hull_component(Component component,Float3 metric){if(auto points=collision_hull_component_points(component)){for(auto& p:*points)p=multiply_components(p,metric);component.properties["points"]=encode_collision_hull_points(*points);}return component;}
} // namespace dve
