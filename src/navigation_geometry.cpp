#include "dve/navigation_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dve {
namespace {

[[nodiscard]] bool bounds_overlap(const NavigationBounds& a,const NavigationBounds& b) noexcept {
    return a.minimum.x<=b.maximum.x&&a.maximum.x>=b.minimum.x&&
           a.minimum.y<=b.maximum.y&&a.maximum.y>=b.minimum.y&&
           a.minimum.z<=b.maximum.z&&a.maximum.z>=b.minimum.z;
}

[[nodiscard]] NavigationBounds quad_bounds(Float3 a,Float3 c) noexcept {
    return {{std::min(a.x,c.x),std::min(a.y,c.y),std::min(a.z,c.z)},
            {std::max(a.x,c.x),std::max(a.y,c.y),std::max(a.z,c.z)}};
}

} // namespace

std::vector<NavigationTriangle> navigation_triangles_from_polygon_asset(
    const CookedPolygonAsset& asset,const NavigationGeometrySettings& settings){
    const auto validation=validate_polygon_asset(asset);
    if(!validation)throw std::invalid_argument("cannot build navigation from invalid polygon asset: "+validation.message);
    std::vector<NavigationTriangle> out;out.reserve(asset.indices.size()/3U);
    for(std::size_t i=0;i+2U<asset.indices.size();i+=3U){
        NavigationTriangle triangle;triangle.vertices={
            transform_point(settings.worldTransform,asset.vertices[asset.indices[i]].position),
            transform_point(settings.worldTransform,asset.vertices[asset.indices[i+1U]].position),
            transform_point(settings.worldTransform,asset.vertices[asset.indices[i+2U]].position)};
        triangle.area=settings.area;triangle.flags=settings.flags;triangle.sourceId=settings.sourceIdBase+i/3U;out.push_back(triangle);
    }
    return out;
}

std::vector<NavigationTriangle> navigation_triangles_from_voxel_object(
    const VoxelObject& object,float voxelSizeMeters,const NavigationGeometrySettings& settings,
    std::optional<NavigationBounds> worldRegion){
    if(!(voxelSizeMeters>0.0F)||!std::isfinite(voxelSizeMeters))throw std::invalid_argument("voxelSizeMeters must be finite and positive");
    std::vector<NavigationTriangle> out;out.reserve(static_cast<std::size_t>(object.occupied_voxel_count()/2U));
    std::uint64_t source=settings.sourceIdBase;
    for(const auto& entry:object.bricks()){
        const auto dense=entry.second.materials();
        for(std::uint16_t index=0;index<kBrickVoxelCount;++index){
            if(dense[index]==kAirMaterial)continue;
            const Int3 local=local_from_index_unchecked(index);const Int3 voxel=global_from_local(entry.first,local);
            if(object.occupied_at({voxel.x,voxel.y+1,voxel.z}))continue;
            const float x0=static_cast<float>(voxel.x)*voxelSizeMeters,x1=static_cast<float>(voxel.x+1)*voxelSizeMeters;
            const float y=static_cast<float>(voxel.y+1)*voxelSizeMeters;
            const float z0=static_cast<float>(voxel.z)*voxelSizeMeters,z1=static_cast<float>(voxel.z+1)*voxelSizeMeters;
            const Float3 a=transform_point(settings.worldTransform,{x0,y,z0});
            const Float3 b=transform_point(settings.worldTransform,{x1,y,z0});
            const Float3 c=transform_point(settings.worldTransform,{x1,y,z1});
            const Float3 d=transform_point(settings.worldTransform,{x0,y,z1});
            if(worldRegion&&!bounds_overlap(quad_bounds(a,c),*worldRegion))continue;
            out.push_back({{a,c,b},settings.area,settings.flags,source++});
            out.push_back({{a,d,c},settings.area,settings.flags,source++});
        }
    }
    return out;
}

} // namespace dve
