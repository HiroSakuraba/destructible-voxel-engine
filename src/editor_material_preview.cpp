#include "dve/editor_material_preview.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_material_inspection.hpp"
#include "dve/render/scene_lighting.hpp"
#include <atomic>
#include <bit>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
namespace dve::editor {
CookedPolygonAsset material_preview_fixture(const VoxelMaterialDefinition& material,MaterialPreviewShape shape){
    CookedPolygonAsset a;a.objectId=1;a.materials={material};a.materialBindings.resize(1);
    if(shape==MaterialPreviewShape::Sphere){
        constexpr unsigned lat=20,lon=40;
        for(unsigned y=0;y<=lat;++y)for(unsigned x=0;x<=lon;++x){const float t=3.14159265F*float(y)/lat,p=6.2831853F*float(x)/lon;
            PolygonVertex v;v.normal={std::sin(t)*std::cos(p),std::cos(t),std::sin(t)*std::sin(p)};
            v.texcoord={float(x)/lon,float(y)/lat};v.texcoord1=v.texcoord;v.tangent={-std::sin(p),0,std::cos(p),1};
            v.position={v.normal.x*.5F,v.normal.y*.5F+.55F,v.normal.z*.5F};a.vertices.push_back(v);}
        for(unsigned y=0;y<lat;++y)for(unsigned x=0;x<lon;++x){const unsigned i=y*(lon+1)+x,j=(y+1)*(lon+1)+x;
            if(y>0)a.indices.insert(a.indices.end(),{i,i+1,j});
            if(y+1<lat)a.indices.insert(a.indices.end(),{j,i+1,j+1});}
        a.submeshes={{"Sphere",0,unsigned(a.indices.size()),0}};refresh_editor_polygon(a);
    }else{
        a=make_editor_polygon_cube(1,1);
        for(auto& v:a.vertices){v.position.x-=.5F;v.position.y+=.05F;v.position.z-=.5F;
            if(shape==MaterialPreviewShape::Patch)v.position.z*=.04F;
            else if(shape==MaterialPreviewShape::Wedge)v.position.z*=.05F+.7F*(v.position.x+.5F);
        }
        if(shape==MaterialPreviewShape::RoundedBox){
            // Subdivide the cube's faces; clamp to an inner box then project a rounded shell.
            CookedPolygonAsset rounded;rounded.objectId=1;rounded.materials={material};rounded.materialBindings.resize(1);
            for(unsigned face=0;face<6;++face){const unsigned axis=face/2;const float sign=face%2?1.F:-1.F;
                const unsigned start=unsigned(rounded.vertices.size());
                for(unsigned y=0;y<=12;++y)for(unsigned x=0;x<=12;++x){float p[3]{};p[axis]=sign*.5F;p[(axis+1)%3]=float(x)/12-.5F;p[(axis+2)%3]=float(y)/12-.5F;
                    Float3 inner{std::clamp(p[0],-.36F,.36F),std::clamp(p[1],-.36F,.36F),std::clamp(p[2],-.36F,.36F)};
                    Float3 n{p[0]-inner.x,p[1]-inner.y,p[2]-inner.z};const float inv=1/std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);n={n.x*inv,n.y*inv,n.z*inv};
                    PolygonVertex v;v.normal=n;v.texcoord={float(x)/12,float(y)/12};v.texcoord1=v.texcoord;v.position={inner.x+.14F*n.x,inner.y+.14F*n.y+.55F,inner.z+.14F*n.z};rounded.vertices.push_back(v);}
                for(unsigned y=0;y<12;++y)for(unsigned x=0;x<12;++x){const unsigned i=start+y*13+x;
                    if(sign>0)rounded.indices.insert(rounded.indices.end(),{i,i+1,i+13,i+1,i+14,i+13});
                    else rounded.indices.insert(rounded.indices.end(),{i,i+13,i+1,i+1,i+13,i+14});}}
            rounded.submeshes={{"Rounded box",0,unsigned(rounded.indices.size()),0}};a=std::move(rounded);refresh_editor_polygon(a);
        }else refresh_editor_polygon(a,true);
        a.materials={material};a.materialBindings.resize(1);
    }
    a.contentHash=polygon_asset_content_hash(a);return a;
}
struct MaterialPreviewWorker::Impl {
    struct Job{CookedPolygonAsset asset;MaterialPreviewShape shape{};std::uint64_t key{},generation{};std::optional<CookedPolygonAsset> baked;render::MaterialTextureBakeSettings settings;MaterialInspectionChannel channel{};MaterialInspectionView view{};};
    mutable std::mutex mutex;std::condition_variable cv;std::optional<Job> pending;
    std::atomic<std::uint64_t> generation{0};std::atomic<bool> stop{false};
    bool working{};std::shared_ptr<const MaterialPreviewImage> current;
    std::map<std::uint64_t,std::shared_ptr<const MaterialPreviewImage>> cache;std::deque<std::uint64_t> order;
    std::optional<std::uint64_t> requestedKey;std::thread worker;
    Impl():worker([this]{run();}){}
    ~Impl(){stop=true;++generation;cv.notify_all();worker.join();}
    void run(){
        while(!stop){Job j;{std::unique_lock lock(mutex);cv.wait(lock,[&]{return stop||pending.has_value();});if(stop)return;j=std::move(*pending);pending.reset();working=true;}
            auto image=std::make_shared<MaterialPreviewImage>();image->rgb.resize(image->size*image->size);
            try{
                if(j.baked)*image=render_material_inspection(j.asset,*j.baked,j.settings,j.channel,j.view,[&]{return generation!=j.generation||stop;});
                else {
#if defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
                auto sample=j.asset;
                auto floor=make_editor_polygon_plane(2,8);floor.materials[0].baseColor={.45F,.45F,.45F,1};
                // A high contrast chart behind the sample makes refraction and frosting visible.
                CookedPolygonAsset chart;chart.objectId=3;VoxelMaterialDefinition white,blue;white.baseColor={.8F,.8F,.8F,1};blue.baseColor={.025F,.1F,.25F,1};chart.materials={white,blue};chart.materialBindings.resize(2);
                for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){const unsigned start=unsigned(chart.vertices.size()),first=unsigned(chart.indices.size());
                    for(const auto corner:std::array<Float2,4>{{{0,0},{1,0},{1,1},{0,1}}}){PolygonVertex v;v.position={-.8F+(float(x)+corner.x)*.2F,-.1F+(float(y)+corner.y)*.2F,-1.1F};v.normal={0,0,1};chart.vertices.push_back(v);}
                    chart.indices.insert(chart.indices.end(),{start,start+1,start+2,start,start+2,start+3});chart.submeshes.push_back({"Chart",first,6,(x+y)%2});}
                // The sample is 10 cm across. Keep optical absorption and grain in real metres.
                for(auto* mesh:{&sample,&floor,&chart}){for(auto& v:mesh->vertices){v.position.x*=.1F;v.position.y*=.1F;v.position.z*=.1F;}refresh_editor_polygon(*mesh);}
                const std::array<render::PolygonRenderInstance,3> objects{{{1,&sample},{2,&floor},{3,&chart}}};
                RenderEnvironment e;e.sunIntensity=0;e.pathTraceBounces=8;e.pathTraceSamples=32;e.localLightSamples=2;e.bloomIntensity=0;
                if(std::any_of(sample.materials.begin(),sample.materials.end(),[](const auto& m){return m.shadingModel==MaterialShadingModel::Subsurface&&m.subsurfaceScatterDistanceMeters>0;}))e.pathTraceSamples=128;
                e.skyColor={.4F,.4F,.4F};e.groundColor={.12F,.12F,.12F};
                SceneLight key;key.kind=SceneLightKind::Quad;key.positionMeters={-.13F,.26F,.18F};key.direction={0,-.7F,-.7F};key.edgeU={.16F,0,0};key.edgeV={0,.08F,-.08F};key.intensity=5;key.twoSided=true;e.lights={key};
                render::SceneLighting lighting({},objects,e,1);
                const Float3 origin{0,.105F,.27F};const float tilt=.5F/2.7F;const float inv=1/std::sqrt(1+tilt*tilt);
                for(unsigned y=0;y<image->size&&generation==j.generation&&!stop;++y)for(unsigned x=0;x<image->size;++x){
                    const float px=(2*(float(x)+.5F)/float(image->size)-1)*.31F,py=(1-2*(float(y)+.5F)/float(image->size))*.31F;
                    const Float3 dir{px,py*inv-tilt*inv,-inv-py*tilt*inv};
                    auto c=lighting.camera_transport(origin,dir,5,y*image->size+x+1).sum();
                    const auto tone=[](float v){v=(std::max)(0.F,v);v=v/(1+v);return std::uint32_t(std::clamp(std::pow(v,1/2.2F)*255,0.F,255.F));};
                    image->rgb[y*image->size+x]=(tone(c.x)<<16)|(tone(c.y)<<8)|tone(c.z);
                }
#else
                image->error="CPU studio unavailable: build with DVE_BUILD_RHI=ON";
#endif
                }
            }catch(const std::exception& ex){image->error=ex.what();}
            {std::lock_guard lock(mutex);working=false;if(generation==j.generation&&!stop){current=image;cache[j.key]=image;order.push_back(j.key);while(order.size()>64){cache.erase(order.front());order.pop_front();}}}
        }
    }
};
MaterialPreviewWorker::MaterialPreviewWorker():impl_(std::make_unique<Impl>()){}
MaterialPreviewWorker::~MaterialPreviewWorker()=default;
void MaterialPreviewWorker::request(const VoxelMaterialDefinition& m,MaterialPreviewShape shape){
    auto fixture=material_preview_fixture(m,shape);request(fixture,0,shape);
}
void MaterialPreviewWorker::request(const CookedPolygonAsset& asset,std::uint32_t slot,MaterialPreviewShape shape){
    if(slot>=asset.materials.size()||slot>=asset.materialBindings.size()){cancel();return;}
    auto fixture=material_preview_fixture(asset.materials[slot],shape);
    fixture.materials=asset.materials;fixture.materialBindings=asset.materialBindings;
    fixture.images=asset.images;fixture.textures=asset.textures;fixture.samplers=asset.samplers;
    for(auto& sub:fixture.submeshes)sub.materialIndex=slot;
    fixture.contentHash=polygon_asset_content_hash(fixture);
    const auto key=fixture.contentHash^std::uint64_t(unsigned(shape)+1)*0x7391eac51ULL; // renderer/studio revision 1
    std::lock_guard lock(impl_->mutex);if(impl_->requestedKey==key)return;impl_->requestedKey=key;
    const auto generation=++impl_->generation;impl_->pending.reset();
    if(auto it=impl_->cache.find(key);it!=impl_->cache.end()){impl_->current=it->second;return;}
    impl_->current.reset();Impl::Job job;job.asset=std::move(fixture);job.shape=shape;job.key=key;job.generation=generation;impl_->pending=std::move(job);impl_->cv.notify_one();
}
void MaterialPreviewWorker::request_inspection(const CookedPolygonAsset& source,const CookedPolygonAsset& baked,const render::MaterialTextureBakeSettings& settings,MaterialInspectionChannel channel,MaterialInspectionView view){
    auto key=polygon_asset_content_hash(source)^std::rotl(polygon_asset_content_hash(baked),17)^std::uint64_t(unsigned(channel)+1)*0x7391eac51ULL^std::uint64_t(unsigned(view)+1)*0x91eac5173ULL;
    const float pose[]{settings.pose.position.x,settings.pose.position.y,settings.pose.position.z,settings.pose.rotation.x,settings.pose.rotation.y,settings.pose.rotation.z,settings.pose.rotation.w,settings.metersPerUnit};
    for(float value:pose)key=(key^std::bit_cast<std::uint32_t>(value))*1099511628211ULL;
    for(auto value:{settings.targetUv,settings.materialIndex,settings.paddingPixels,settings.resolution})key=(key^value)*1099511628211ULL;
    std::lock_guard lock(impl_->mutex);if(impl_->requestedKey==key)return;impl_->requestedKey=key;const auto generation=++impl_->generation;impl_->pending.reset();impl_->current.reset();
    Impl::Job job;job.asset=source;job.baked=baked;job.settings=settings;job.channel=channel;job.view=view;job.key=key;job.generation=generation;impl_->pending=std::move(job);impl_->cv.notify_one();
}
void MaterialPreviewWorker::cancel(){std::lock_guard lock(impl_->mutex);++impl_->generation;impl_->pending.reset();impl_->requestedKey.reset();impl_->current.reset();}
std::shared_ptr<const MaterialPreviewImage> MaterialPreviewWorker::image()const{std::lock_guard lock(impl_->mutex);return impl_->current;}
bool MaterialPreviewWorker::busy()const{std::lock_guard lock(impl_->mutex);return impl_->working||impl_->pending.has_value();}
}
