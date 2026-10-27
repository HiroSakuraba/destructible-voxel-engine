#include "dve_test_skip.hpp"
#include "dve/render/live_environment_renderer.hpp"
#include "dve/render/live_optical_scene.hpp"
#include "dve/render/optical_scene.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/rhi/null_device.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>

using namespace dve;using namespace dve::render;using namespace dve::rhi;
namespace {
std::string error;
void check(bool b,const std::string& why){if(!b)throw std::runtime_error(why+": "+error);}
Float3 add3(Float3 a,Float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 sub(Float3 a,Float3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 mul(Float3 a,Float3 b){return {a.x*b.x,a.y*b.y,a.z*b.z};}
Float3 scale3(Float3 a,float b){return {a.x*b,a.y*b,a.z*b};}
Float3 cross3(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
VoxelMaterialDefinition glass(float ior=1.5F,Float3 color={1,1,1},float distance=0){VoxelMaterialDefinition m;m.name="GPU clear glass";m.transmission=1;m.transparent=true;m.roughness=0;m.indexOfRefraction=ior;m.attenuationColor=color;m.attenuationDistanceMeters=distance;return m;}
void finish(CookedPolygonAsset& a){a.contentHash=polygon_asset_content_hash(a);check(bool(validate_polygon_asset(a)),"Test mesh validation");}
CookedPolygonAsset box(VoxelMaterialDefinition m,Float3 center,Float3 half){
    CookedPolygonAsset a;a.materials={m};a.materialBindings.resize(1);
    std::array<Float3,8> points{};for(unsigned i=0;i<8;++i)points[i]={center.x+(i&1?half.x:-half.x),center.y+(i&2?half.y:-half.y),center.z+(i&4?half.z:-half.z)};
    for(auto face:std::array<std::array<unsigned,4>,6>{{{0,2,3,1},{4,5,7,6},{0,4,6,2},{1,3,7,5},{0,1,5,4},{2,6,7,3}}}){
        const auto n=normalize(cross3(sub(points[face[1]],points[face[0]]),sub(points[face[2]],points[face[0]])));const auto base=static_cast<unsigned>(a.vertices.size());
        for(unsigned j=0;j<4;++j){PolygonVertex v;v.position=points[face[j]];v.normal=n;a.vertices.push_back(v);}for(unsigned i:{0U,1U,2U,0U,2U,3U})a.indices.push_back(base+i);
    }a.bounds={sub(center,half),add3(center,half)};a.submeshes={{"closed slab",0,static_cast<unsigned>(a.indices.size()),0}};finish(a);return a;
}
CookedPolygonAsset sphere(VoxelMaterialDefinition m,Float3 center,float radius){
    std::vector<Float3> points{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    std::vector<std::array<unsigned,3>> faces{{0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    for(unsigned level=0;level<3;++level){std::map<std::pair<unsigned,unsigned>,unsigned> cache;std::vector<std::array<unsigned,3>> next;
        const auto midpoint=[&](unsigned a,unsigned b){const auto key=std::minmax(a,b);const std::pair<unsigned,unsigned> k{key.first,key.second};if(cache.contains(k))return cache[k];const auto index=static_cast<unsigned>(points.size());points.push_back(normalize(add3(points[a],points[b])));cache[k]=index;return index;};
        for(auto f:faces){const auto a=midpoint(f[0],f[1]),b=midpoint(f[1],f[2]),c=midpoint(f[2],f[0]);next.insert(next.end(),{{f[0],a,c},{a,f[1],b},{c,b,f[2]},{a,b,c}});}faces=std::move(next);}
    CookedPolygonAsset a;a.materials={m};a.materialBindings.resize(1);
    for(auto p:points){PolygonVertex v;v.position=add3(center,scale3(p,radius));v.normal=p;a.vertices.push_back(v);}
    for(auto f:faces){if(dot(cross3(sub(points[f[1]],points[f[0]]),sub(points[f[2]],points[f[0]])),points[f[0]])<0)std::swap(f[1],f[2]);a.indices.insert(a.indices.end(),f.begin(),f.end());}
    a.bounds={sub(center,{radius,radius,radius}),add3(center,{radius,radius,radius})};a.submeshes={{"closed smooth sphere",0,static_cast<unsigned>(a.indices.size()),0}};finish(a);return a;
}
CookedPolygonAsset checker(float z,bool uniform=false){
    CookedPolygonAsset a;a.materials.resize(2);a.materialBindings.resize(2);
    for(unsigned i=0;i<2;++i){a.materials[i].baseColor={0,0,0,1};a.materials[i].emissive=uniform?Float3{.3F,.3F,.3F}:i?Float3{.1F,.6F,.8F}:Float3{.8F,.15F,.1F};}
    for(unsigned y=0;y<20;++y)for(unsigned x=0;x<20;++x){const auto base=static_cast<unsigned>(a.vertices.size()),first=static_cast<unsigned>(a.indices.size());
        for(auto p:std::array<Float3,4>{{{float(x)*.3F-3,float(y)*.3F-3,z},{float(x+1)*.3F-3,float(y)*.3F-3,z},{float(x+1)*.3F-3,float(y+1)*.3F-3,z},{float(x)*.3F-3,float(y+1)*.3F-3,z}}}){PolygonVertex v;v.position=p;v.normal={0,0,1};a.vertices.push_back(v);}
        for(unsigned i:{0U,1U,2U,0U,2U,3U})a.indices.push_back(base+i);a.submeshes.push_back({"checker",first,6,(x+y)%2});}
    a.bounds={{-3,-3,z},{3,3,z}};finish(a);return a;
}
void builder_checks(){
    auto a=box(glass(),{0,0,0},{1,1,1});LiveOpticalInstance instance{&a};LiveOpticalSceneData scene;
    check(build_live_optical_scene(std::span(&instance,1),scene,&error),"Build closed optical scene");check(scene.triangles.size()==12&&scene.nodes[0].escape==scene.nodes.size(),"Stackless BVH contract");
    check(set_live_optical_camera(scene,{0,0,0},&error)&&scene.settings.mediumCount==1&&scene.settings.media[0]==1,"Camera inside volume");
    check(set_live_optical_camera(scene,{0,0,3},&error)&&scene.settings.mediumCount==0,"Camera outside volume");
    {NullDevice d;LiveOpticalSceneGpu gpu(d);check(gpu.prepare(std::span(&instance,1),{0,0,3},&error),"GPU scene upload");check(gpu.builds()==1,"First scene build");
        check(gpu.prepare(std::span(&instance,1),{0,0,0},&error)&&gpu.builds()==1&&gpu.data().settings.mediumCount==1,"Camera update reuses BVH");
        auto edited=a;edited.materials[0].indexOfRefraction=1.333F;finish(edited);instance.asset=&edited;
        check(gpu.prepare(std::span(&instance,1),{0,0,0},&error)&&gpu.builds()==2,"Edited optics rebuilds scene");
        gpu.clear();check(d.statistics().buffersCreated==d.statistics().buffersDestroyed&&d.statistics().bindGroupsCreated==d.statistics().bindGroupsDestroyed,"Optical resource lifetime");instance.asset=&a;}
    for(unsigned bad=0;bad<8;++bad){auto invalid=a;
        if(bad==0){invalid.indices.resize(33);invalid.submeshes[0].indexCount=33;}
        if(bad==1)invalid.materials[0].roughness=.2F;
        if(bad==2){invalid.images={{"map","image/raw",1,1,{255,255,255,255}}};invalid.textures={{"map",0,{}}};invalid.materialBindings[0].baseColor.texture=0;}
        if(bad==3)invalid.materials[0].blendMode=MaterialBlendMode::Masked;
        if(bad==4)for(unsigned i=0;i<invalid.indices.size();i+=3)std::swap(invalid.indices[i],invalid.indices[i+1]);
        if(bad==6)invalid.materials[0].emissive={1,0,0};
        if(bad==7){invalid.materials[0].transmission=0;invalid.materials[0].shadingModel=MaterialShadingModel::Unlit;}
        finish(invalid);instance.asset=&invalid;if(bad==5)instance.objectToWorld[0]=2;
        check(!build_live_optical_scene(std::span(&instance,1),scene,&error),"Unsupported optical scene refused");instance.objectToWorld[0]=1;error.clear();}
    {   // Disjoint components of one material: nesting must follow the component containing the camera.
        auto inner=box(glass(1.333F),{0,0,0},{.2F,.2F,.2F});const auto remote=box(glass(1.333F),{20,0,0},{3,3,3});
        const auto base=static_cast<std::uint32_t>(inner.vertices.size()),first=static_cast<std::uint32_t>(inner.indices.size());
        inner.vertices.insert(inner.vertices.end(),remote.vertices.begin(),remote.vertices.end());
        for(auto i:remote.indices)inner.indices.push_back(base+i);inner.submeshes[0].indexCount=first+static_cast<std::uint32_t>(remote.indices.size());
        inner.bounds={{-.2F,-3,-3},{23,3,3}};finish(inner);
        auto outer=box(glass(1.5F),{0,0,0},{1,1,1});
        std::array<LiveOpticalInstance,2> nested{{{&inner},{&outer}}};
        check(build_live_optical_scene(nested,scene,&error)&&scene.volumes.size()==3,"Per-component dielectric volumes");
        check(set_live_optical_camera(scene,{0,0,0},&error)&&scene.settings.mediumCount==2&&scene.settings.media[0]==2&&scene.settings.media[1]==1,"Initial media ordered by containing component");
        check(set_live_optical_camera(scene,{.5F,0,0},&error)&&scene.settings.mediumCount==1&&scene.settings.media[0]==2,"Camera only in enclosing component");
        check(set_live_optical_camera(scene,{20,0,0},&error)&&scene.settings.mediumCount==1&&scene.settings.media[0]==1,"Camera in remote component");
    }
    std::cout<<"Optical scene closure, containment, cache/edit/lifetime and refusal checks passed\n";
}
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary|std::ios::ate);check(bool(in),"Missing shader "+p.string());const auto size=in.tellg();std::vector<std::byte> bytes(static_cast<std::size_t>(size));in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),size);check(bool(in),"Shader read");return bytes;}
void math_probes(VulkanDevice& d,const std::filesystem::path& root){
    struct Probe {float cosI,n1,n2,distance;Float3 color;float reference;};static_assert(sizeof(Probe)==32);
    std::vector<Probe> probes;for(float n1:{1.F,1.333F,1.5F,2.4F})for(float n2:{1.F,1.333F,1.5F})for(float cosine:{0.F,.001F,.2F,.5F,.7F,.9F,1.F})probes.push_back({cosine,n1,n2,.27F,{0,.25F,1},.1F});
    const auto make=[&](std::size_t size){BufferDesc desc;desc.bytes=size;desc.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;desc.memory=MemoryDomain::Upload;desc.initialState=ResourceState::ShaderRead;return d.create_buffer(desc,&error);};
    auto input=make(probes.size()*sizeof(Probe)),output=make(probes.size()*sizeof(Float4)*2);check(input&&output,"Dielectric buffers");check(d.write_buffer(input,0,std::as_bytes(std::span(probes)),&error),"Dielectric upload");
    BindGroupLayoutDesc ld;ld.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};const auto layout=d.create_bind_group_layout(ld,&error);BindGroupDesc gd;gd.layout=layout;gd.entries={{0,input,{},0,probes.size()*sizeof(Probe),{}},{1,output,{},0,probes.size()*sizeof(Float4)*2,{}}};const auto group=d.create_bind_group(gd,&error);
    ComputePipelineDesc pd;pd.bytecode=load(root/"clear_glass_reference.spv");pd.bindGroupLayouts={layout};const auto pipeline=d.create_compute_pipeline(pd,&error);check(layout&&group&&pipeline,"Dielectric compute pipeline");auto commands=d.begin_commands(QueueKind::Compute,"Dielectric conformance",&error);check(d.bind_compute_bind_group(commands,0,group,&error)&&d.dispatch(commands,pipeline,static_cast<unsigned>((probes.size()+63)/64),1,1,&error),"Dielectric dispatch");auto fence=d.submit(commands,&error);check(fence&&d.fence_complete(fence),"Dielectric fence");
    std::vector<Float4> actual(probes.size()*2);check(d.read_buffer(output,0,std::as_writable_bytes(std::span(actual)),&error),"Dielectric readback");
    for(unsigned i=0;i<probes.size();++i){const auto& p=probes[i];const auto t=refract_direction({std::sqrt(1-p.cosI*p.cosI),0,-p.cosI},{0,0,1},p.n1/p.n2);const auto& a=actual[i*2];
        check(std::abs(a.x-dielectric_fresnel(p.cosI,p.n1,p.n2))<2e-5F,"Exact dielectric Fresnel");check((a.w>.5F)==bool(t),"Total internal reflection classification");if(t)check(std::abs(a.y-t->x)+std::abs(a.z-t->z)<2e-4F,"Snell direction");
        auto m=glass(p.n1,p.color,p.reference);const auto expected=medium_transmittance(m,p.distance,1);const auto& absorption=actual[i*2+1];check(std::abs(absorption.x-expected.x)+std::abs(absorption.y-expected.y)+std::abs(absorption.z-expected.z)<1e-5F,"Beer-Lambert absorption");}
    check(d.destroy_compute_pipeline(pipeline,&error)&&d.destroy_bind_group(group,&error)&&d.destroy_bind_group_layout(layout,&error)&&d.destroy_buffer(input,&error)&&d.destroy_buffer(output,&error),"Dielectric cleanup");
    std::cout<<probes.size()<<" GPU Fresnel/Snell/TIR/absorption probes passed\n";
}
// Independent reference uses the existing CPU OpticalScene and optics functions,
// with an independent hierarchy and a larger total path budget.
Float3 reference(const OpticalScene& scene,std::span<const CookedPolygonAsset> assets,Float3 camera,Float3 direction,std::vector<unsigned> initial,Float3 environment={}){
    struct Path{Float3 origin,direction,weight;unsigned depth;std::vector<unsigned> media;};
    std::vector<Path> paths{{camera,direction,{1,1,1},0,std::move(initial)}};Float3 sum{};unsigned budget=0;
    while(!paths.empty()&&budget++<2048){auto p=std::move(paths.back());paths.pop_back();if(p.depth>=24||(std::max)({p.weight.x,p.weight.y,p.weight.z})<1e-6F)continue;
        const auto hit=scene.trace(p.origin,p.direction,1000);if(!hit){sum=add3(sum,mul(p.weight,environment));continue;}
        if(!p.media.empty())p.weight=mul(p.weight,medium_transmittance(assets[p.media.back()].materials[0],hit->distance,1));
        const auto& m=*hit->material;
        if(m.transmission<=0){sum=add3(sum,mul(p.weight,m.emissive));continue;}
        const unsigned id=static_cast<unsigned>(hit->polygonInstance);const auto Ng=hit->geometricNormal;const bool entering=dot(p.direction,Ng)<0;const auto opposing=entering?Ng:scale3(Ng,-1);
        auto N=entering?hit->shadingNormal:scale3(hit->shadingNormal,-1);if(dot(N,p.direction)>=0)N=opposing;
        if(!entering){check(!p.media.empty()&&p.media.back()==id,"Reference nested exit");}
        const float n1=p.media.empty()?1:assets[p.media.back()].materials[0].indexOfRefraction;
        const float n2=entering?m.indexOfRefraction:p.media.size()>1?assets[p.media[p.media.size()-2]].materials[0].indexOfRefraction:1;
        auto T=refract_direction(p.direction,N,n1/n2),R=std::optional<Float3>(reflect_direction(p.direction,N));
        if(dot(*R,opposing)<=0||(T&&dot(*T,opposing)>=0)){N=opposing;T=refract_direction(p.direction,N,n1/n2);R=reflect_direction(p.direction,N);}
        const float F=T?dielectric_fresnel(-dot(p.direction,N),n1,n2):1;
        const auto offset=[&](Float3 d){return add3(hit->position,scale3(Ng,dot(d,Ng)>=0?1e-5F:-1e-5F));};
        paths.push_back({offset(*R),*R,scale3(p.weight,F),p.depth+1,p.media});
        if(T){auto media=p.media;if(entering)media.push_back(id);else media.pop_back();paths.push_back({offset(*T),*T,scale3(p.weight,(1-F)*(n1/n2)*(n1/n2)),p.depth+1,std::move(media)});}
    }return sum;
}
void live_images(VulkanDevice& d,const std::filesystem::path& root,const std::filesystem::path& output){
    IblBakeSettings bake;bake.irradianceResolution=4;bake.specularResolution=8;bake.specularMipLevels=4;bake.sampleCount=16;bake.brdfResolution=8;
    auto environment=make_default_environment_lighting_asset(bake);
    for(auto& level:environment.baked.specularPrefilter.levels)std::fill(level.texels.begin(),level.texels.end(),Float3{.2F,.3F,.4F});
    environment.recompute_content_hash();EnvironmentLightingGpuResources lighting;check(upload_environment_lighting(d,environment,lighting,&error),"Glass IBL");camera::CameraPose camera;camera.position={0,0,2};camera.target={0,0,0};camera.lens.farPlaneMeters=10;
    CascadedShadowSettings ss;ss.cascadeCount=1;ss.cascadeResolution=64;ss.maximumDistanceMeters=5;auto plan=make_cascaded_shadow_plan(camera,{0,1,1},ss);CascadedShadowAtlasResources atlas;check(create_cascaded_shadow_atlas(d,plan,atlas,&error),"Glass atlas");
    LiveEnvironmentShaderBytecode code;code.materialVertex=load(root/"live_ibl_material_vs.spv");code.materialFragment=load(root/"live_ibl_material_ps.spv");code.clearGlassFragment=load(root/"live_clear_glass_ps.spv");code.skyboxVertex=load(root/"live_environment_skybox_vs.spv");code.skyboxFragment=load(root/"live_environment_skybox_ps.spv");code.shadowVertex=load(root/"live_csm_caster_vs.spv");code.shadowFragment=load(root/"live_csm_caster_ps.spv");
    LiveEnvironmentRendererResources renderer;check(create_live_environment_renderer(d,lighting,atlas,code,256,TextureFormat::RGBA8Unorm,renderer,&error),"Glass renderer");
    constexpr unsigned width=64;TextureDesc td;td.width=td.height=width;td.usage=TextureUsage::RenderTarget|TextureUsage::CopySource;td.initialState=ResourceState::RenderTarget;auto target=d.create_texture(td,&error);td.format=TextureFormat::D32Float;td.usage=TextureUsage::DepthStencil;td.initialState=ResourceState::DepthWrite;auto depth=d.create_texture(td,&error);check(target&&depth,"Glass targets");
    unsigned comparisons=0;float maximum=0;
    for(unsigned variant=0;variant<9;++variant){std::vector<CookedPolygonAsset> assets;std::vector<unsigned> initial;Float3 eye{0,0,2};
        auto material=glass(variant==2?1.333F:1.5F,variant==1?Float3{.18F,.65F,.25F}:Float3{1,1,1},variant==1?.4F:0);
        if(variant<3)assets.push_back(sphere(material,{0,0,.5F},.35F));
        if(variant==3)assets.push_back(box(material,{0,0,.5F},{.45F,.45F,.15F}));
        if(variant==4){assets.push_back(box(glass(1.5F,{.5F,.8F,.3F},.5F),{0,0,2},{.5F,.5F,.5F}));initial={0};}
        if(variant==5){assets.push_back(box(material,{0,0,.5F},{.5F,.47F,.4F}));assets.push_back(box(glass(1.333F),{0,0,.5F},{.3F,.28F,.2F}));}
        if(variant==6){assets.push_back(box(glass(1.5F),{0,0,2},{.5F,.48F,.5F}));assets.push_back(box(glass(1.333F,{.4F,.7F,.9F},.5F),{0,0,2},{.25F,.23F,.25F}));initial={0,1};}
        if(variant==7)assets.push_back(box(material,{0,0,.5F},{.45F,.43F,.15F}));
        if(variant==8){ // Camera inside absorbing glass with an opaque receiver before the exit surface.
            assets.push_back(box(glass(1.5F,{.5F,.8F,.3F},.5F),{0,0,2},{1,1,1}));initial={0};
            VoxelMaterialDefinition receiver;receiver.baseColor={0,0,0,1};receiver.emissive={.9F,.6F,.3F};assets.push_back(box(receiver,{0,0,1.4F},{.3F,.3F,.1F}));}
        if(variant<7)assets.push_back(checker(-.6F,variant>=3));if(variant<3)assets.push_back(checker(2.6F));
        std::vector<std::unique_ptr<MeshRhiMirror>> mirrors;std::vector<LivePolygonDraw> draws;std::vector<OpticalPolygonInstance> instances;
        for(unsigned i=0;i<assets.size();++i){assets[i].objectId=i+1;finish(assets[i]);mirrors.push_back(std::make_unique<MeshRhiMirror>(d));check(mirrors.back()->upload(assets[i],&error),"Glass fixture upload");draws.push_back({mirrors.back().get(),&assets[i],1});instances.push_back({i+1,&assets[i],{},true});}
        OpticalScene cpu({},instances,1);
        std::array<float,64> constants{};const float f=2.41421356F,far=10,near=.01F;
        constants[0]=constants[5]=f;constants[10]=-far/(far-near);constants[11]=-1;constants[14]=far*(eye.z-near)/(far-near);constants[15]=eye.z;constants[19]=3;if(variant==7){constants[17]=.7F;constants[18]=1;}
        LiveEnvironmentFrameDesc frame;frame.colorTarget=target;frame.depthTarget=depth;frame.width=frame.height=width;frame.frameConstants=std::as_bytes(std::span(constants));frame.polygonDraws=draws;frame.cameraWorldPosition=eye;frame.drawSkybox=false;frame.drawShadowCasters=true;
        LiveEnvironmentFrameStats stats;FenceHandle fence;check(record_live_environment_frame(d,lighting,plan,atlas,renderer,frame,stats,&fence,&error),"Clear glass live frame");check(fence&&d.fence_complete(fence),"Glass fence");std::uint64_t submeshes=0;for(const auto& asset:assets)submeshes+=asset.submeshes.size();
        check(stats.clearGlassDraws==(!initial.empty()?submeshes:variant==7?1:assets.size()-1-(variant<3?1:0))&&stats.opticalSceneBuilds==1,"Automatic optical draw dispatch");
        std::uint64_t opaqueTriangles=0;
        for(const auto& asset:assets)for(const auto& submesh:asset.submeshes)
            if(asset.materials[submesh.materialIndex].transmission<=0)opaqueTriangles+=submesh.indexCount/3;
        check(stats.shadowTriangles==opaqueTriangles,"Clear glass must not become an opaque shadow caster");
        std::vector<std::byte> pixels(width*width*4);check(d.read_texture(target,0,0,pixels,width*4,&error),"Glass readback");std::vector<unsigned char> expected(width*width*3),diff(expected.size());unsigned checked=0;float worst=0;
        for(unsigned y=0;y<width;++y)for(unsigned x=0;x<width;++x){const auto direction=normalize(Float3{(2*(float(x)+.5F)/width-1)/f,(2*(float(y)+.5F)/width-1)/f,-1});
            const auto primary=cpu.trace(eye,direction,1000);const bool optical=primary&&(primary->material->transmission>0||!initial.empty());
            auto value=optical?reference(cpu,assets,eye,direction,initial,variant==7?Float3{.28F,.42F,.56F}:Float3{}):primary?primary->material->emissive:Float3{};
            const std::array<float,3> channels{value.x,value.y,value.z};
            for(unsigned c=0;c<3;++c){const float actual=std::to_integer<unsigned>(pixels[(y*width+x)*4+c])/255.F,ref=std::clamp(channels[c],0.F,1.F),delta=std::abs(actual-ref);expected[(y*width+x)*3+c]=static_cast<unsigned char>(std::lround(ref*255));diff[(y*width+x)*3+c]=static_cast<unsigned char>((std::min)(255.F,delta*255*8));
                // Offset grid probes avoid symmetric box-edge and raster silhouette ties; all pixels are retained in evidence.
                if(optical&&x%7==3&&y%7==4){check(delta<.025F,"CPU/GPU glass mismatch variant "+std::to_string(variant)+" pixel "+std::to_string(x)+","+std::to_string(y)+" channel "+std::to_string(c)+" actual "+std::to_string(actual)+" expected "+std::to_string(ref));worst=(std::max)(worst,delta);++comparisons;}}
            if(optical)++checked;}
        check(checked>100,"Glass must occupy visible pixels");maximum=(std::max)(maximum,worst);
        check(record_live_environment_frame(d,lighting,plan,atlas,renderer,frame,stats,&fence,&error)&&fence&&d.fence_complete(fence)&&stats.opticalSceneBuilds==0,"Identical frame reuses optical hierarchy");
        if(!output.empty()){std::filesystem::create_directories(output);for(unsigned kind=0;kind<3;++kind){std::ofstream ppm(output/("glass-"+std::to_string(variant)+(kind==0?".gpu.ppm":kind==1?".cpu.ppm":".diff.ppm")),std::ios::binary);ppm<<"P6\n64 64\n255\n";for(unsigned i=0;i<width*width;++i)for(unsigned c=0;c<3;++c){const unsigned char v=kind==0?std::to_integer<unsigned char>(pixels[i*4+c]):kind==1?expected[i*3+c]:diff[i*3+c];ppm.write(reinterpret_cast<const char*>(&v),1);}}}
        for(const auto& a:assets)check(renderer.mainMaterials->invalidate_asset(a.contentHash,&error)&&renderer.shadowMaterials->invalidate_asset(a.contentHash,&error),"Glass material invalidation");
    }
    std::cout<<"9 live glass scenes; "<<comparisons<<" independent CPU/GPU channel comparisons; max error "<<maximum<<'\n';
    check(destroy_live_environment_renderer(d,renderer,&error)&&d.destroy_texture(target,&error)&&d.destroy_texture(depth,&error),"Glass renderer cleanup");check(destroy_cascaded_shadow_atlas(d,atlas,&error)&&destroy_environment_lighting(d,lighting,&error),"Glass lighting cleanup");
}
}
int main(int argc,char** argv){try{
    builder_checks();
#ifdef DVE_SURFACE_SHADER_DIR
    const std::filesystem::path root=argc>1?argv[1]:DVE_SURFACE_SHADER_DIR;
#else
    if(argc<2){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error("DXC shaders required");return dve::testing::skip("GPU clear glass test: configure DXC or pass a shader directory");}const std::filesystem::path root=argv[1];
#endif
    VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("GPU clear glass test: Vulkan unavailable");}
    math_probes(d,root);live_images(d,root,argc>2?argv[2]:std::filesystem::path{});std::cout<<"GPU clear glass passed\n";return dve::testing::exit_code(false);
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
