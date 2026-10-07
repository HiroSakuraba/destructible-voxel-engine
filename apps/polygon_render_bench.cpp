// A bounded repeated-mesh fixture for before/after CPU renderer measurements.
#include "dve/render/polygon_renderer.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <iostream>

int main(){
    using namespace dve;using namespace dve::render;
    CookedPolygonAsset asset;
    asset.materials.push_back({});asset.materialBindings.push_back({});asset.materialBindings[0].baseColor.texture=0;
    asset.materialBindings[0].doubleSided=true;
    asset.vertices={{{-1,-1,0},{0,0,1}},{{1,-1,0},{0,0,1}},{{1,1,0},{0,0,1}},{{-1,1,0},{0,0,1}}};
    asset.indices={0,1,2,0,2,3};asset.submeshes.push_back({"quad",0,6,0});asset.bounds={{-1,-1,0},{1,1,0}};
    PolygonImage image;image.width=512;image.height=512;image.rgba8.resize(512U*512U*4U,255);
    asset.images.push_back(std::move(image));asset.samplers.push_back({});asset.textures.push_back({"texture",0,0});
    asset.contentHash=polygon_asset_content_hash(asset);
    std::vector<PolygonRenderInstance> instances;
    for(unsigned i=0;i<64;++i)instances.push_back({i+1,&asset});
    PolygonCamera camera;camera.position={0,0,5};camera.target={0,0,0};
    PolygonRenderTarget target;target.resize(32,32);ReferencePolygonRenderer renderer;PolygonRenderCache cache;
    PolygonRenderOptions options;options.preserveExistingDepth=false;options.cache=&cache;
    PolygonRenderStats stats;std::vector<double> times;
    for(unsigned frame=0;frame<13;++frame){
        const auto start=std::chrono::steady_clock::now();stats=renderer.render(instances,camera,{},target,options);
        const auto milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(frame>=3)times.push_back(milliseconds);
    }
    std::sort(times.begin(),times.end());
    std::uint64_t hash=1469598103934665603ULL;
    const auto mix=[&](std::uint64_t value){hash^=value;hash*=1099511628211ULL;};
    for(std::size_t i=0;i<target.depth.size();++i){const auto color=target.hdrColor[i];
        mix(std::bit_cast<std::uint32_t>(color.x));mix(std::bit_cast<std::uint32_t>(color.y));
        mix(std::bit_cast<std::uint32_t>(color.z));mix(std::bit_cast<std::uint32_t>(color.w));
        mix(std::bit_cast<std::uint32_t>(target.depth[i]));mix(target.objectId[i]);mix(target.materialIndex[i]);}
    std::cout<<"{\"instances\":64,\"texture\":\"512x512\",\"median_ms\":"<<times[times.size()/2]
        <<",\"asset_validations\":"<<stats.assetValidations<<",\"mip_chains_built\":"<<stats.mipChainsBuilt
        <<",\"mip_cache_hits\":"<<stats.mipCacheHits<<",\"mip_cache_bytes\":"<<stats.mipCacheBytes
        <<",\"frame_hash\":"<<hash<<"}\n";
}
