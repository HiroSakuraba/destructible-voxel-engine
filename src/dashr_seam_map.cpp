#include "dve/dashr_seam_map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace dve {
namespace {

constexpr float kEpsilon = 1.0e-8F;

bool finite(float value) noexcept { return std::isfinite(value); }

Float2 add(Float2 a, Float2 b) noexcept { return {a.x+b.x,a.y+b.y}; }
Float2 subtract(Float2 a, Float2 b) noexcept { return {a.x-b.x,a.y-b.y}; }
Float2 multiply(Float2 a, float s) noexcept { return {a.x*s,a.y*s}; }
float dot(Float2 a, Float2 b) noexcept { return a.x*b.x+a.y*b.y; }
float length_squared(Float2 a) noexcept { return dot(a,a); }
Float2 normalized(Float2 value) noexcept {
    const float sq=length_squared(value);
    if (!(sq>kEpsilon*kEpsilon) || !finite(sq)) return {};
    return multiply(value,1.0F/std::sqrt(sq));
}

struct QuantizedPoint {
    std::int64_t x{},y{},z{};
    friend auto operator<=>(const QuantizedPoint&,const QuantizedPoint&)=default;
};
struct EdgeKey {
    QuantizedPoint a{},b{};
    friend auto operator<=>(const EdgeKey&,const EdgeKey&)=default;
};
struct EdgeRef {
    Float2 uvA{},uvB{},thirdUv{};
};

bool quantize(Float3 position,float tolerance,QuantizedPoint& out) noexcept {
    const double inverse=1.0/static_cast<double>(tolerance);
    const std::array<double,3> scaled{
        static_cast<double>(position.x)*inverse,
        static_cast<double>(position.y)*inverse,
        static_cast<double>(position.z)*inverse};
    constexpr double limit=static_cast<double>(std::numeric_limits<std::int64_t>::max())*0.5;
    for(double value:scaled) if(!std::isfinite(value)||std::abs(value)>limit) return false;
    out={static_cast<std::int64_t>(std::llround(scaled[0])),
         static_cast<std::int64_t>(std::llround(scaled[1])),
         static_cast<std::int64_t>(std::llround(scaled[2]))};
    return true;
}

float uv_distance_squared(Float2 a,Float2 b) noexcept {
    return length_squared(subtract(a,b));
}

Float2 inward_normal(Float2 a,Float2 b,Float2 third) noexcept {
    const Float2 edge=subtract(b,a);
    Float2 normal=normalized({-edge.y,edge.x});
    if(dot(normal,subtract(third,a))<0.0F) normal=multiply(normal,-1.0F);
    return normal;
}

struct ClosestPoint {
    Float2 point{};
    float parameter{};
    float distanceSquared{};
};

ClosestPoint closest_on_segment(Float2 p,Float2 a,Float2 b) noexcept {
    const Float2 ab=subtract(b,a);
    const float denominator=length_squared(ab);
    if(!(denominator>kEpsilon)) return {a,0.0F,uv_distance_squared(p,a)};
    const float t=std::clamp(dot(subtract(p,a),ab)/denominator,0.0F,1.0F);
    const Float2 q=add(a,multiply(ab,t));
    return {q,t,uv_distance_squared(p,q)};
}

void rasterize_direction(
    DashrSeamMap& map,
    const EdgeRef& source,
    const EdgeRef& destination,
    const DashrSeamCookSettings& settings) {
    const float resolution=static_cast<float>(settings.resolution);
    const float radius=static_cast<float>(settings.radiusPixels)/resolution;
    const float insetRequested=static_cast<float>(settings.destinationInsetPixels)/resolution;
    const Float2 sourceInward=inward_normal(source.uvA,source.uvB,source.thirdUv);
    const Float2 destinationInward=inward_normal(destination.uvA,destination.uvB,destination.thirdUv);
    if(length_squared(sourceInward)<kEpsilon || length_squared(destinationInward)<kEpsilon) {
        ++map.stats.degenerateUvEdgesSkipped;
        return;
    }

    const float minU=std::min(source.uvA.x,source.uvB.x)-radius;
    const float maxU=std::max(source.uvA.x,source.uvB.x)+radius;
    const float minV=std::min(source.uvA.y,source.uvB.y)-radius;
    const float maxV=std::max(source.uvA.y,source.uvB.y)+radius;
    const auto to_min_pixel=[&](float uv){
        return std::clamp(static_cast<int>(std::floor(uv*resolution-0.5F)),
                          0,static_cast<int>(settings.resolution)-1);
    };
    const auto to_max_pixel=[&](float uv){
        return std::clamp(static_cast<int>(std::ceil(uv*resolution-0.5F)),
                          0,static_cast<int>(settings.resolution)-1);
    };
    const int x0=to_min_pixel(minU),x1=to_max_pixel(maxU);
    const int y0=to_min_pixel(minV),y1=to_max_pixel(maxV);
    const float radiusSquared=radius*radius;
    const Float2 destinationEdge=subtract(destination.uvB,destination.uvA);

    for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) {
        const Float2 uv{(static_cast<float>(x)+0.5F)/resolution,
                        (static_cast<float>(y)+0.5F)/resolution};
        const ClosestPoint closest=closest_on_segment(uv,source.uvA,source.uvB);
        if(closest.distanceSquared>radiusSquared) continue;
        const float inwardDistance=dot(subtract(uv,closest.point),sourceInward);
        if(inwardDistance < -0.25F/resolution || inwardDistance > radius) continue;

        const Float2 targetEdgePoint=add(destination.uvA,multiply(destinationEdge,closest.parameter));
        const float destinationAltitude=std::abs(
            dot(subtract(destination.thirdUv,targetEdgePoint),destinationInward));
        if(!(destinationAltitude>kEpsilon)) continue;
        const float inset=std::min(insetRequested,std::max(0.5F/resolution,
                                                           destinationAltitude*0.45F));
        const Float2 target=add(targetEdgePoint,multiply(destinationInward,inset));
        if(target.x<0.0F||target.x>1.0F||target.y<0.0F||target.y>1.0F) continue;

        const float seamValue=std::clamp(1.0F-std::max(0.0F,inwardDistance)/radius,
                                         0.001F,1.0F);
        const std::size_t index=static_cast<std::size_t>(y)*settings.resolution+
                                static_cast<std::size_t>(x);
        if(map.texels[index].z>=seamValue) continue;
        if(map.texels[index].z<=0.0F) ++map.stats.markedTexels;
        map.texels[index]={target.x,target.y,seamValue,1.0F};
    }
}

} // namespace

bool validate_dashr_seam_cook_settings(
    const DashrSeamCookSettings& settings,std::string* error) noexcept {
    const auto fail=[&](const char* message){if(error)*error=message;return false;};
    if(settings.resolution<32U||settings.resolution>2048U||
       settings.radiusPixels==0U||settings.radiusPixels>16U||
       settings.destinationInsetPixels<=settings.radiusPixels||
       settings.destinationInsetPixels>32U)
        return fail("DASHR seam-map pixel settings are invalid");
    if(!finite(settings.positionToleranceMeters)||settings.positionToleranceMeters<=0.0F||
       settings.positionToleranceMeters>0.1F||
       !finite(settings.uvTolerance)||settings.uvTolerance<=0.0F||settings.uvTolerance>0.01F)
        return fail("DASHR seam-map tolerances are invalid");
    if(settings.maximumTriangleEdges==0U)
        return fail("DASHR seam-map edge budget must be nonzero");
    return true;
}

std::optional<DashrSeamMap> cook_dashr_seam_map(
    const CookedPolygonAsset& asset,
    const DashrSeamCookSettings& settings,
    std::string* error) {
    std::string validation;
    if(!validate_dashr_seam_cook_settings(settings,&validation)) {
        if (error) *error = validation;
        return std::nullopt;
    }
    const auto valid=validate_polygon_asset(asset);
    if(!valid) {if(error)*error=valid.message;return std::nullopt;}
    const std::uint64_t edgeCount=asset.indices.size();
    if(edgeCount>settings.maximumTriangleEdges) {
        if(error)*error="DASHR seam-map triangle-edge budget exceeded";
        return std::nullopt;
    }

    DashrSeamMap result;
    result.resolution=settings.resolution;
    result.texels.assign(static_cast<std::size_t>(settings.resolution)*settings.resolution,
                         Float4{0.0F,0.0F,-1.0F,0.0F});
    result.stats.triangleEdges=edgeCount;

    std::map<EdgeKey,std::vector<EdgeRef>> groups;
    for(std::size_t triangle=0U;triangle<asset.indices.size();triangle+=3U) {
        const std::array<std::uint32_t,3> ids{
            asset.indices[triangle],asset.indices[triangle+1U],asset.indices[triangle+2U]};
        for(std::size_t edge=0U;edge<3U;++edge) {
            const auto ia=ids[edge],ib=ids[(edge+1U)%3U],ic=ids[(edge+2U)%3U];
            const auto& va=asset.vertices[ia];
            const auto& vb=asset.vertices[ib];
            const auto& vc=asset.vertices[ic];
            QuantizedPoint qa,qb;
            if(!quantize(va.position,settings.positionToleranceMeters,qa)||
               !quantize(vb.position,settings.positionToleranceMeters,qb)) {
                if(error)*error="DASHR seam-map position quantization overflowed";
                return std::nullopt;
            }
            EdgeRef ref{va.texcoord,vb.texcoord,vc.texcoord};
            EdgeKey key;
            if(qb<qa) {
                key={qb,qa};
                std::swap(ref.uvA,ref.uvB);
            } else key={qa,qb};
            groups[key].push_back(ref);
        }
    }
    result.stats.geometricEdgeGroups=groups.size();

    const float uvToleranceSquared=settings.uvTolerance*settings.uvTolerance;
    for(const auto& [key,edges]:groups) {
        (void)key;
        if(edges.size()==1U) continue;
        if(edges.size()!=2U) {
            ++result.stats.nonManifoldGroupsSkipped;
            continue;
        }
        const EdgeRef& a=edges[0];
        const EdgeRef& b=edges[1];
        if(uv_distance_squared(a.uvA,b.uvA)<=uvToleranceSquared &&
           uv_distance_squared(a.uvB,b.uvB)<=uvToleranceSquared) continue;
        if(length_squared(subtract(a.uvB,a.uvA))<=kEpsilon ||
           length_squared(subtract(b.uvB,b.uvA))<=kEpsilon) {
            ++result.stats.degenerateUvEdgesSkipped;
            continue;
        }
        ++result.stats.seamPairs;
        rasterize_direction(result,a,b,settings);
        rasterize_direction(result,b,a,settings);
    }
    return result;
}

} // namespace dve
