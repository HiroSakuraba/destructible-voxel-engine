#include "dve_test_skip.hpp"
#include "dve/gpu_material_surface_detail.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/layered_surface.hpp"
#include <chrono>
#include "dve/editor_material_presets.hpp"
#include "dve/editor_material_preview.hpp"
#include "dve/environment_lighting.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/render/live_environment_renderer.hpp"
#include "dve/rhi/vulkan_device.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dve;using namespace dve::render;using namespace dve::rhi;
namespace {
std::string error;
void check(bool b,const std::string& why){if(!b)throw std::runtime_error(why+" "+error);}
std::vector<std::byte> load(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary|std::ios::ate);check(bool(in),"Missing compiled shader "+p.string());auto size=in.tellg();std::vector<std::byte> b(static_cast<std::size_t>(size));in.seekg(0);in.read(reinterpret_cast<char*>(b.data()),size);check(bool(in),"Shader read");return b;}
BufferHandle buffer(VulkanDevice& d,std::size_t size){BufferDesc b;b.bytes=size;b.usage=BufferUsage::Storage|BufferUsage::CopySource|BufferUsage::CopyDestination;b.memory=MemoryDomain::Upload;b.initialState=ResourceState::ShaderRead;auto out=d.create_buffer(b,&error);check(bool(out),"Buffer");return out;}
void analytic_conformance(VulkanDevice& d,const std::filesystem::path& root){
    std::vector<GpuMaterialSurfaceDetail> records;std::vector<Float4> positions,expected;
    for(unsigned pattern=0;pattern<=8;++pattern)for(unsigned seed:{1U,7U,65520U,0xffffffffU})
    for(float width:{0.F,.0001F,.003F,.3F})for(float sign:{-1.F,1.F}){
        MaterialSurfaceDetail m;m.pattern=SurfacePattern(pattern);m.periodMeters=.013F;m.rotationRadians={.2F,-.4F,.7F};m.seed=seed;m.grainStretch=31;
        Float3 p{sign*.0123F,.038F,-.024F};auto s=sample_surface_pattern(m,p,width);
        records.push_back(pack_gpu_material_surface_detail(m));positions.push_back({p.x,p.y,p.z,width});expected.push_back({s.value,s.gradientPerMeter.x,s.gradientPerMeter.y,s.gradientPerMeter.z});
    }
    auto a=buffer(d,records.size()*sizeof(records[0])),b=buffer(d,positions.size()*sizeof(positions[0])),out=buffer(d,expected.size()*sizeof(expected[0]));
    check(d.write_buffer(a,0,std::as_bytes(std::span(records)),&error)&&d.write_buffer(b,0,std::as_bytes(std::span(positions)),&error),"Compute upload");
    BindGroupLayoutDesc layout;layout.bindings={{0,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{1,BindingType::StorageBufferReadOnly,ShaderStage::Compute},{2,BindingType::StorageBufferReadWrite,ShaderStage::Compute}};
    auto l=d.create_bind_group_layout(layout,&error);check(bool(l),"Compute layout");BindGroupDesc desc;desc.layout=l;desc.entries={{0,a,{},0,records.size()*sizeof(records[0]),{}},{1,b,{},0,positions.size()*sizeof(positions[0]),{}},{2,out,{},0,expected.size()*sizeof(expected[0]),{}}};auto g=d.create_bind_group(desc,&error);check(bool(g),"Compute group");
    ComputePipelineDesc pipeline;pipeline.bytecode=load(root/"material_surface_detail_reference.spv");pipeline.bindGroupLayouts={l};auto p=d.create_compute_pipeline(pipeline,&error);check(bool(p),"Compute pipeline");auto commands=d.begin_commands(QueueKind::Compute,"surface CPU/GPU conformance",&error);check(bool(commands),"Commands");
    check(d.bind_compute_bind_group(commands,0,g,&error)&&d.dispatch(commands,p,static_cast<unsigned>((records.size()+63)/64),1,1,&error),"Dispatch");auto fence=d.submit(commands,&error);check(fence&&d.fence_complete(fence),"Compute fence");
    std::vector<Float4> actual(expected.size());check(d.read_buffer(out,0,std::as_writable_bytes(std::span(actual)),&error),"Compute readback");float worst=0;
    for(std::size_t i=0;i<actual.size();++i){std::array<float,4> x{actual[i].x,actual[i].y,actual[i].z,actual[i].w},y{expected[i].x,expected[i].y,expected[i].z,expected[i].w};
        for(unsigned c=0;c<4;++c){const float tolerance=c==0?.004F:.8F+.015F*std::abs(y[c]);check(std::isfinite(x[c])&&std::abs(x[c]-y[c])<tolerance,"GPU analytic mismatch case "+std::to_string(i)+" channel "+std::to_string(c)+" got "+std::to_string(x[c])+" expected "+std::to_string(y[c]));if(c==0)worst=(std::max)(worst,std::abs(x[c]-y[c]));}}
    std::cout<<records.size()<<" analytic CPU/GPU samples; maximum value error "<<worst<<'\n';
    check(d.destroy_compute_pipeline(p,&error)&&d.destroy_bind_group(g,&error)&&d.destroy_bind_group_layout(l,&error),"Compute cleanup");for(auto h:{a,b,out})check(d.destroy_buffer(h,&error),"Buffer cleanup");
}
CookedPolygonAsset panel(const VoxelMaterialDefinition& material){CookedPolygonAsset a;a.objectId=1;a.materials={material};a.materialBindings.resize(1);
    for(auto p:std::array<Float3,4>{{{-.012F,-.012F,.2F},{.012F,-.012F,.2F},{.012F,.012F,.2F},{-.012F,.012F,.2F}}}){PolygonVertex v;v.position=p;v.normal={0,0,1};v.tangent={1,0,0,1};v.texcoord={(p.x+.012F)/.024F,(p.y+.012F)/.024F};v.texcoord1={v.texcoord.y*1.7F+.3F,v.texcoord.x*.4F-.2F};a.vertices.push_back(v);}
    a.indices={0,1,2,0,2,3};a.submeshes={{"panel",0,6,0}};a.bounds={{-.012F,-.012F,.2F},{.012F,.012F,.2F}};a.contentHash=polygon_asset_content_hash(a);return a;}
std::array<float,16> rigid_matrix(const RigidTransform& t){const auto x=transform_vector(t,{1,0,0}),y=transform_vector(t,{0,1,0}),z=transform_vector(t,{0,0,1});return {x.x,x.y,x.z,0,y.x,y.y,y.z,0,z.x,z.y,z.z,0,t.position.x,t.position.y,t.position.z,1};}
void live_pixels(VulkanDevice& d,const std::filesystem::path& root,const std::filesystem::path& output){
    IblBakeSettings settings;settings.irradianceResolution=4;settings.specularResolution=8;settings.specularMipLevels=4;settings.sampleCount=16;settings.brdfResolution=8;EnvironmentLightingGpuResources lighting;check(upload_environment_lighting(d,make_default_environment_lighting_asset(settings),lighting,&error),"IBL");
    camera::CameraPose camera;camera.position={0,0,1};camera.target={0,0,0};camera.lens.farPlaneMeters=10;
    CascadedShadowSettings shadows;shadows.cascadeCount=1;shadows.cascadeResolution=64;shadows.maximumDistanceMeters=5;auto plan=make_cascaded_shadow_plan(camera,{0,1,1},shadows);CascadedShadowAtlasResources atlas;check(create_cascaded_shadow_atlas(d,plan,atlas,&error),"Atlas");
    LiveEnvironmentShaderBytecode code;code.materialVertex=load(root/"live_ibl_material_vs.spv");code.materialFragment=load(root/"live_ibl_material_ps_color.spv");code.skyboxVertex=load(root/"live_environment_skybox_vs.spv");code.skyboxFragment=load(root/"live_environment_skybox_ps.spv");code.shadowVertex=load(root/"live_csm_caster_vs.spv");code.shadowFragment=load(root/"live_csm_caster_ps.spv");LiveEnvironmentRendererResources renderer;check(create_live_environment_renderer(d,lighting,atlas,code,256,TextureFormat::RGBA8Unorm,renderer,&error),"Live renderer");
    constexpr unsigned width=48;TextureDesc texture;texture.width=width;texture.height=width;texture.usage=TextureUsage::RenderTarget|TextureUsage::CopySource;texture.initialState=ResourceState::RenderTarget;auto target=d.create_texture(texture,&error);texture.format=TextureFormat::D32Float;texture.usage=TextureUsage::DepthStencil;texture.initialState=ResourceState::DepthWrite;auto depth=d.create_texture(texture,&error);check(target&&depth,"Targets");
    std::array<float,64> constants{};constants[0]=constants[5]=1/.012F;constants[10]=constants[15]=1;constants[17]=2;constants[19]=3;
    auto catalog=editor::MaterialPresetCatalog::starter();unsigned checks=0;
    for(unsigned pass=0;pass<3;++pass){
    if(pass){check(destroy_live_environment_renderer(d,renderer,&error),"Switch debug shader");
        code.materialFragment=load(root/(pass==1?"live_ibl_material_ps_normal.spv":"live_ibl_material_ps.spv"));
        check(create_live_environment_renderer(d,lighting,atlas,code,256,TextureFormat::RGBA8Unorm,renderer,&error),"Switch live pipeline");}

    for(const auto& preset:catalog.entries())if(preset.defaults.surfaceDetail.pattern!=SurfacePattern::NoPattern){
      for(unsigned variant=0;variant<(pass==2?1U:5U);++variant){auto material=preset.defaults;material.surfaceDetail.space=SurfaceDetailSpace(variant<4?variant:0);if(variant==4){material.surfaceDetail.periodMeters=.0001F;material.surfaceDetail.bumpHeightMeters=(std::min)(material.surfaceDetail.bumpHeightMeters,.000025F);}
        auto asset=panel(material);MeshRhiMirror mirror(d);check(mirror.upload(asset,&error),"Mesh upload");LivePolygonDraw draw{&mirror,&asset,1};RigidTransform pose;if(variant==1){pose.position={.013F,.021F,0};pose.rotation=quaternion_from_axis_angle({0,0,1},.4F);draw.objectToWorld=rigid_matrix(pose);}
        // Cancel the object pose in the camera so the same pixels sample the same local positions.
        const auto inverse=relative_rigid_transform(pose,{});auto vp=rigid_matrix(inverse);for(unsigned col=0;col<4;++col){vp[col*4]*=1/.012F;vp[col*4+1]*=1/.012F;}std::copy(vp.begin(),vp.end(),constants.begin());
        LiveEnvironmentFrameDesc frame;frame.colorTarget=target;frame.depthTarget=depth;frame.width=width;frame.height=width;frame.frameConstants=std::as_bytes(std::span(constants));frame.polygonDraws=std::span(&draw,1);frame.cameraWorldPosition={0,0,1};frame.drawShadowCasters=false;frame.drawSkybox=false;LiveEnvironmentFrameStats stats;FenceHandle fence;check(record_live_environment_frame(d,lighting,plan,atlas,renderer,frame,stats,&fence,&error),"Live frame");check(fence&&d.fence_complete(fence),"Live fence");std::vector<std::byte> pixels(width*width*4);check(d.read_texture(target,0,0,pixels,width*4,&error),"Live readback");
        if(pass<2)for(unsigned y=3;y<width-3;y+=7)for(unsigned x=3;x<width-3;x+=7){Float3 p{(2*(float(x)+.5F)/width-1)*.012F,(2*(float(y)+.5F)/width-1)*.012F,.2F};Float2 uv{(p.x+.012F)/.024F,(p.y+.012F)/.024F},uv1{uv.y*1.7F+.3F,uv.x*.4F-.2F};float footprint=.024F/width;if(material.surfaceDetail.space==SurfaceDetailSpace::UV0)footprint=1.F/width*material.surfaceDetail.uvTileMeters;if(material.surfaceDetail.space==SurfaceDetailSpace::UV1)footprint=1.7F/width*material.surfaceDetail.uvTileMeters;
          auto expected=material;Float3 normal=transform_vector(pose,{0,0,1});apply_material_surface_detail(expected,normal,{p,transform_point(pose,p),uv,uv1,true,pose,transform_vector(pose,{1,0,0}),transform_vector(pose,{0,1,0}),footprint});const std::array<float,4> channels{pass==1?normal.x*.5F+.5F:expected.baseColor.x,pass==1?normal.y*.5F+.5F:expected.baseColor.y,pass==1?normal.z*.5F+.5F:expected.baseColor.z,expected.roughness};for(unsigned c=0;c<4;++c){const float actual=std::to_integer<unsigned>(pixels[(y*width+x)*4+c])/255.F;check(std::abs(actual-channels[c])<.009F,"Live CPU/GPU pixel mismatch "+preset.id+" variant "+std::to_string(variant)+" channel "+std::to_string(c)+" actual "+std::to_string(actual)+" expected "+std::to_string(channels[c]));}++checks;}
        if(!output.empty()&&variant==0){std::filesystem::create_directories(output);std::ofstream ppm(output/(preset.id+(pass==0?".color.ppm":pass==1?".normal.ppm":".beauty.ppm")),std::ios::binary);ppm<<"P6\n"<<width<<' '<<width<<"\n255\n";for(unsigned i=0;i<width*width;++i)ppm.write(reinterpret_cast<const char*>(pixels.data()+i*4),3);}
        if(pass==2){
          for(unsigned shape=0;shape<3;++shape){
            auto fixture=editor::material_preview_fixture(material,shape==0?editor::MaterialPreviewShape::Sphere:shape==1?editor::MaterialPreviewShape::RoundedBox:editor::MaterialPreviewShape::Patch);
            for(auto& v:fixture.vertices){v.position.x*=.02F;v.position.y=(v.position.y-.55F)*.02F;v.position.z*=.02F;}
            fixture.bounds={{-.01F,-.01F,-.01F},{.01F,.01F,.01F}};fixture.contentHash=polygon_asset_content_hash(fixture);
            check(mirror.upload(fixture,&error),"Curved fixture upload");draw.asset=&fixture;
            RigidTransform tilt;if(shape==2)tilt.rotation=quaternion_from_axis_angle({0,1,0},1.2F);
            draw.objectToWorld=rigid_matrix(tilt);constants.fill(0);constants[0]=constants[5]=1/.012F;constants[10]=-1;constants[14]=.5F;constants[15]=1;constants[16]=shape==1?.9F:0;constants[17]=2;constants[19]=3;
            frame.cameraWorldPosition={0,0,.3F};
            check(record_live_environment_frame(d,lighting,plan,atlas,renderer,frame,stats,&fence,&error),"Curved/grazing live frame");check(fence&&d.fence_complete(fence),"Curved fence");
            check(d.read_texture(target,0,0,pixels,width*4,&error),"Curved readback");
            unsigned visible=0;for(unsigned i=0;i<width*width;++i)if(std::to_integer<unsigned>(pixels[i*4])+std::to_integer<unsigned>(pixels[i*4+1])+std::to_integer<unsigned>(pixels[i*4+2])>3)++visible;
            check(visible>100,"Beauty pass must shade visible fixture pixels");
            if(!output.empty()){std::ofstream ppm(output/(preset.id+(shape==0?".sphere.ppm":shape==1?".rounded.ppm":".grazing.ppm")),std::ios::binary);ppm<<"P6\n"<<width<<' '<<width<<"\n255\n";for(unsigned i=0;i<width*width;++i)ppm.write(reinterpret_cast<const char*>(pixels.data()+i*4),3);}
            check(renderer.mainMaterials->invalidate_asset(fixture.contentHash,&error),"Curved invalidation");
          }
        }
        check(renderer.mainMaterials->invalidate_asset(asset.contentHash,&error),"Invalidate recipe");mirror.reset();
      }
    }
    }
    // Exercise actual sampled images as well as procedural recipes: color is sRGB,
    // roughness and normal channels are data, and normals must never be gamma decoded.
    VoxelMaterialDefinition mappedMaterial;
    mappedMaterial.baseColor={.5F,.7F,.9F,1};mappedMaterial.roughness=.8F;
    auto mapped=panel(mappedMaterial);
    mapped.images={{"color","image/raw",1,1,{128,64,200,255}},
                   {"roughness","image/raw",1,1,{255,128,0,255}},
                   {"normal","image/raw",1,1,{200,128,230,255}}};
    for(unsigned i=0;i<3;++i)mapped.textures.push_back({"map",i,{}});
    mapped.materialBindings[0].baseColor.texture=0;
    mapped.materialBindings[0].baseColor.colorSpace=PolygonTextureColorSpace::Srgb;
    mapped.materialBindings[0].metallicRoughness.texture=1;
    mapped.materialBindings[0].normal.texture=2;
    mapped.contentHash=polygon_asset_content_hash(mapped);
    MeshRhiMirror mappedMirror(d);check(mappedMirror.upload(mapped,&error),"Mapped fixture upload");
    LivePolygonDraw mappedDraw{&mappedMirror,&mapped,1};
    constants.fill(0);constants[0]=constants[5]=1/.012F;constants[10]=constants[15]=1;
    LiveEnvironmentFrameDesc mappedFrame;mappedFrame.colorTarget=target;mappedFrame.depthTarget=depth;
    mappedFrame.width=mappedFrame.height=width;mappedFrame.frameConstants=std::as_bytes(std::span(constants));
    mappedFrame.polygonDraws=std::span(&mappedDraw,1);mappedFrame.drawShadowCasters=false;mappedFrame.drawSkybox=false;
    const auto linear=[](float v){return v<=.04045F?v/12.92F:std::pow((v+.055F)/1.055F,2.4F);};
    const auto dataNormal=normalize(Float3{200.F/255*2-1,128.F/255*2-1,230.F/255*2-1});
    for(unsigned pass=0;pass<2;++pass){
        check(destroy_live_environment_renderer(d,renderer,&error),"Mapped pipeline cleanup");
        code.materialFragment=load(root/(pass?"live_ibl_material_ps_normal.spv":"live_ibl_material_ps_color.spv"));
        check(create_live_environment_renderer(d,lighting,atlas,code,256,TextureFormat::RGBA8Unorm,renderer,&error),"Mapped pipeline");
        LiveEnvironmentFrameStats stats;FenceHandle fence;
        check(record_live_environment_frame(d,lighting,plan,atlas,renderer,mappedFrame,stats,&fence,&error),"Mapped frame");
        check(fence&&d.fence_complete(fence),"Mapped fence");
        std::vector<std::byte> pixels(width*width*4);
        check(d.read_texture(target,0,0,pixels,width*4,&error),"Mapped readback");
        const std::array<float,4> expected=pass?std::array<float,4>{dataNormal.x*.5F+.5F,dataNormal.y*.5F+.5F,dataNormal.z*.5F+.5F,.8F*128/255}:
            std::array<float,4>{.5F*linear(128.F/255),.7F*linear(64.F/255),.9F*linear(200.F/255),.8F*128/255};
        for(unsigned c=0;c<4;++c)check(std::abs(std::to_integer<unsigned>(pixels[(24*width+24)*4+c])/255.F-expected[c])<.009F,"GPU sampled color/data channel mismatch");
        check(renderer.mainMaterials->invalidate_asset(mapped.contentHash,&error),"Mapped invalidation");
    }
    mappedMirror.reset();
    // Codex 4364355458 (#202): a flat normal map under triplanar mapping must reproduce the
    // geometric normal (renderer still holds the normal-debug fragment shader from above).
    for(const auto mode:{MaterialMappingMode::WorldTriplanar,MaterialMappingMode::ObjectTriplanar}){
        const Float3 tilted=normalize(Float3{.45F,-.55F,.7F});
        auto neutral=panel(mappedMaterial);neutral.images={{"normal","image/raw",1,1,{128,128,255,255}}};
        neutral.textures.push_back({"map",0,{}});neutral.materialBindings[0].normal.texture=0;neutral.materialBindings[0].normalScale=1.5F;
        neutral.materialBindings[0].mapping.mappingMode=mode;neutral.materialBindings[0].mapping.triplanarBlendSharpness=4;
        for(auto& v:neutral.vertices)v.normal=tilted;neutral.contentHash=polygon_asset_content_hash(neutral);
        MeshRhiMirror neutralMirror(d);check(neutralMirror.upload(neutral,&error),"Neutral fixture upload");
        LivePolygonDraw neutralDraw{&neutralMirror,&neutral,1};mappedFrame.polygonDraws=std::span(&neutralDraw,1);
        LiveEnvironmentFrameStats stats;FenceHandle fence;
        check(record_live_environment_frame(d,lighting,plan,atlas,renderer,mappedFrame,stats,&fence,&error),"Neutral frame");
        check(fence&&d.fence_complete(fence),"Neutral fence");
        std::vector<std::byte> pixels(width*width*4);check(d.read_texture(target,0,0,pixels,width*4,&error),"Neutral readback");
        const std::array<float,3> expected{tilted.x*.5F+.5F,tilted.y*.5F+.5F,tilted.z*.5F+.5F};
        for(unsigned c=0;c<3;++c){const float actual=std::to_integer<unsigned>(pixels[(24*width+24)*4+c])/255.F;
            check(std::abs(actual-expected[c])<.012F,"Neutral triplanar normal map perturbed the GPU normal, channel "+std::to_string(c)+" actual "+std::to_string(actual)+" expected "+std::to_string(expected[c]));}
        check(renderer.mainMaterials->invalidate_asset(neutral.contentHash,&error),"Neutral invalidation");neutralMirror.reset();
    }
    std::cout<<"Neutral triplanar normal maps keep the geometric normal\n";
    std::cout<<"Sampled sRGB color, linear roughness and raw normal channels passed\n";
    std::cout<<checks<<" live shaded CPU/GPU pixel comparisons\n";
    check(d.destroy_texture(target,&error)&&d.destroy_texture(depth,&error),"Targets cleanup");check(destroy_live_environment_renderer(d,renderer,&error),"Renderer cleanup");check(destroy_cascaded_shadow_atlas(d,atlas,&error),"Atlas cleanup");check(destroy_environment_lighting(d,lighting,&error),"IBL cleanup");
}
// Canonical live pipelines, real sampled textures and depth readback. Constant UV
// probes isolate channel/wrap/filter selection; varying UV probes exercise implicit LOD.
void texture_pixels(VulkanDevice& d,const std::filesystem::path& root,const std::filesystem::path& output){
    IblBakeSettings settings;settings.irradianceResolution=4;settings.specularResolution=8;
    settings.specularMipLevels=4;settings.sampleCount=16;settings.brdfResolution=8;
    EnvironmentLightingGpuResources lighting;
    check(upload_environment_lighting(d,make_default_environment_lighting_asset(settings),lighting,&error),"Texture IBL");
    camera::CameraPose camera;camera.position={0,0,1};camera.target={0,0,0};camera.lens.farPlaneMeters=10;
    CascadedShadowSettings shadows;shadows.cascadeCount=1;shadows.cascadeResolution=64;shadows.maximumDistanceMeters=5;
    auto plan=make_cascaded_shadow_plan(camera,{0,0,1},shadows);
    auto& cascade=plan.cascades[0];cascade.snappedCenter={0,0,.2F};cascade.radiusMeters=.012F;
    cascade.texelSizeMeters=.024F/64;cascade.lightRight={1,0,0};cascade.lightUp={0,1,0};cascade.lightForward={0,0,1};
    cascade.minimumLightDepth=0;cascade.maximumLightDepth=1;
    CascadedShadowAtlasResources atlas;check(create_cascaded_shadow_atlas(d,plan,atlas,&error),"Texture atlas");
    LiveEnvironmentShaderBytecode code;code.materialVertex=load(root/"live_ibl_material_vs.spv");
    code.skyboxVertex=load(root/"live_environment_skybox_vs.spv");code.skyboxFragment=load(root/"live_environment_skybox_ps.spv");
    code.shadowVertex=load(root/"live_csm_caster_vs.spv");code.shadowFragment=load(root/"live_csm_caster_ps.spv");
    LiveEnvironmentRendererResources renderer;std::string activeMode;
    constexpr unsigned width=64;
    TextureDesc desc;desc.width=desc.height=width;desc.usage=TextureUsage::RenderTarget|TextureUsage::CopySource;desc.initialState=ResourceState::RenderTarget;
    auto target=d.create_texture(desc,&error);desc.format=TextureFormat::D32Float;desc.usage=TextureUsage::DepthStencil;desc.initialState=ResourceState::DepthWrite;
    auto depth=d.create_texture(desc,&error);check(target&&depth,"Texture targets");
    std::array<float,64> constants{};constants[0]=constants[5]=1/.012F;constants[10]=constants[15]=1;
    unsigned cases=0,shadowPixels=0;
    auto render=[&](CookedPolygonAsset asset,const std::string& mode,bool caster=false){
        if(activeMode!=mode){if(!activeMode.empty())check(destroy_live_environment_renderer(d,renderer,&error),"Texture pipeline cleanup");
            code.materialFragment=load(root/("live_ibl_material_ps_"+mode+".spv"));
            check(create_live_environment_renderer(d,lighting,atlas,code,256,TextureFormat::RGBA8Unorm,renderer,&error),"Texture pipeline");activeMode=mode;}
        asset.contentHash=polygon_asset_content_hash(asset);MeshRhiMirror mirror(d);check(mirror.upload(asset,&error),"Texture mesh");LivePolygonDraw draw{&mirror,&asset,1};
        LiveEnvironmentFrameDesc frame;frame.colorTarget=target;frame.depthTarget=depth;frame.width=frame.height=width;
        frame.frameConstants=std::as_bytes(std::span(constants));frame.polygonDraws=std::span(&draw,1);frame.drawSkybox=false;frame.drawShadowCasters=caster;
        LiveEnvironmentFrameStats stats;FenceHandle fence;
        check(record_live_environment_frame(d,lighting,plan,atlas,renderer,frame,stats,&fence,&error),"Texture frame");check(fence&&d.fence_complete(fence),"Texture fence");
        std::vector<std::byte> pixels(width*width*4);check(d.read_texture(target,0,0,pixels,width*4,&error),"Texture readback");
        if(caster){std::vector<float> shadow(width*width);check(d.read_texture(atlas.dynamicDepthAtlas,0,0,std::as_writable_bytes(std::span(shadow)),width*sizeof(float),&error),"Cutout depth readback");
            unsigned lit=0,holes=0;
            for(unsigned y=2;y<width-2;++y)for(unsigned x=2;x<width-2;++x){
                const bool visible=std::to_integer<unsigned>(pixels[(y*width+x)*4])>0;
                const bool casts=shadow[((width-1-y)*width+x)]<.9F;
                check(visible==casts,"Main/shadow cutout coverage differs at "+std::to_string(x)+","+std::to_string(y));
                visible?++lit:++holes;++shadowPixels;}
            check(lit>100&&holes>100,"Cutout fixture must contain visible and discarded pixels");
            if(!output.empty()){std::filesystem::create_directories(output);
                for(unsigned kind=0;kind<3;++kind){std::ofstream ppm(output/("cutout-"+std::to_string(cases)+(kind==0?".main.ppm":kind==1?".shadow.ppm":".diff.ppm")),std::ios::binary);ppm<<"P6\n64 64\n255\n";
                    for(unsigned y=0;y<width;++y)for(unsigned x=0;x<width;++x){const bool visible=std::to_integer<unsigned>(pixels[(y*width+x)*4])>0,casts=shadow[(width-1-y)*width+x]<.9F;
                        const unsigned char v=(kind==0?visible:kind==1?casts:visible!=casts)?255:0;for(unsigned c=0;c<3;++c)ppm.write(reinterpret_cast<const char*>(&v),1);}}}
        }
        check(renderer.mainMaterials->invalidate_asset(asset.contentHash,&error)&&renderer.shadowMaterials->invalidate_asset(asset.contentHash,&error),"Texture invalidation");
        ++cases;return pixels;
    };
    auto probe=[&](const std::vector<std::byte>& pixels,std::array<float,4> expected,const std::string& name,float tolerance=.009F){
        for(unsigned c=0;c<4;++c){const float actual=std::to_integer<unsigned>(pixels[(32*width+32)*4+c])/255.F;
            check(std::abs(actual-expected[c])<tolerance,name+" channel "+std::to_string(c)+" actual "+std::to_string(actual)+" expected "+std::to_string(expected[c]));}
    };
    VoxelMaterialDefinition material;material.baseColor={1,1,1,1};material.roughness=1;material.metallic=1;material.emissive={1,1,1};
    auto split=panel(material);split.images={{"split","image/raw",2,1,{64,64,64,255,192,192,192,255}}};
    split.samplers={{ImportedWrapMode::Repeat,ImportedWrapMode::Repeat,ImportedTextureFilter::Nearest,ImportedTextureFilter::Nearest}};
    split.textures={{"split",0,0}};
    for(auto& v:split.vertices){v.texcoord={.25F,.5F};v.texcoord1={.75F,.5F};}
    for(unsigned channel=0;channel<2;++channel){auto a=split;auto& b=a.materialBindings[0];
        b.baseColor={0,channel,PolygonTextureColorSpace::Linear};b.metallicRoughness={0,1-channel};b.emissive={0,channel,PolygonTextureColorSpace::Linear};b.opacity={0,channel};
        const float base=(channel?192.F:64.F)/255,other=(channel?64.F:192.F)/255;
        probe(render(a,"color"),{base,base,base,other},"Independent base/MR UV");
        probe(render(a,"emission"),{base,base,base,base},"Independent emissive/opacity UV");
        probe(render(a,"channels"),{other,other,base,1},"Independent data UV");}
    {auto a=split;a.materialBindings[0].baseColor={0,0,PolygonTextureColorSpace::Linear};a.materialBindings[0].mapping.mappingMode=MaterialMappingMode::UV1;
        probe(render(a,"color"),{192.F/255,192.F/255,192.F/255,1},"Global UV1 override");}
    for(unsigned channel=0;channel<2;++channel){auto a=split;
        a.images[0].rgba8={200,128,230,255,55,128,230,255};a.materialBindings[0].normal={0,channel};
        for(auto& v:a.vertices){const float u=(v.position.x+.012F)/.024F,vv=(v.position.y+.012F)/.024F;
            v.texcoord={.25F+(u-.5F)*.05F,vv};v.texcoord1={.75F+(u-.5F)*.05F,vv};}
        const auto n=normalize(Float3{(channel?55.F:200.F)/255*2-1,128.F/255*2-1,230.F/255*2-1});
        probe(render(a,"normal"),{n.x*.5F+.5F,n.y*.5F+.5F,n.z*.5F+.5F,1},"Independent normal UV");}
    // Tangent orientation must survive small UV scales; an absolute determinant
    // epsilon incorrectly drops normal maps on dense meshes and tiny ray cones.
    for(bool layered:{false,true}){auto a=panel(material);
        a.images={{"scaled normal","image/raw",1,1,{200,128,230,255}}};a.textures={{"scaled normal",0,{}}};
        for(auto& v:a.vertices)v.texcoord1={v.texcoord.x*.0001F,v.texcoord.y*.0001F};
        if(layered){a.materials.push_back(material);a.materialBindings.emplace_back();a.materials[0].layers={{1,1,MaterialLayerBlendMode::Lerp,true}};}
        a.materialBindings[layered?1:0].normal={0,1};const auto n=normalize(Float3{200.F/255*2-1,128.F/255*2-1,230.F/255*2-1});
        probe(render(a,"normal"),{n.x*.5F+.5F,n.y*.5F+.5F,n.z*.5F+.5F,1},"Scale-independent normal frame");}
    {MaterialSamplerPolicy policy;policy.overrideFiltering=true;policy.anisotropy=true;policy.requestedMaxAnisotropy=128;
        MaterialResourceResidency pool(d,policy);check(pool.retain_asset(split.contentHash,&error),"Anisotropy retain");
        SamplerHandle sampler;check(pool.ensure_sampler(split,0,sampler,&error),"Actual Vulkan anisotropic sampler");
        check(pool.effective_max_anisotropy()<=(std::max)(1.F,d.capabilities().maxSamplerAnisotropy),"Anisotropy device clamp");
        check(pool.release_asset(split.contentHash,&error),"Anisotropy release");}
    for(auto wrap:{ImportedWrapMode::Repeat,ImportedWrapMode::ClampToEdge,ImportedWrapMode::MirroredRepeat}){
        auto a=split;a.samplers[0].wrapS=wrap;a.materialBindings[0].baseColor={0,0,PolygonTextureColorSpace::Linear};
        for(auto& v:a.vertices)v.texcoord={1.25F,.5F};
        const float expected=(wrap==ImportedWrapMode::Repeat?64.F:192.F)/255;
        probe(render(a,"color"),{expected,expected,expected,1},"Authored wrap mode");}
    // Decode must precede bilinear filtering: black/white at the boundary is 0.5 linear.
    {auto a=split;a.images[0].rgba8={0,0,0,255,255,255,255,255};a.samplers[0].magFilter=ImportedTextureFilter::Linear;
        for(auto& v:a.vertices)v.texcoord={.5F,.5F};a.materialBindings[0].baseColor.texture=0;
        probe(render(a,"color"),{.5F,.5F,.5F,1},"sRGB decode before bilinear");
        a.samplers[0].magFilter=ImportedTextureFilter::Nearest;
        for(auto& v:a.vertices)v.texcoord={.49F,.5F};probe(render(a,"color"),{0,0,0,1},"Nearest magnification");}
    // A highly minified checker reaches the 1x1 level. All color mips average in
    // linear light, while the same source image bound as MR remains raw data.
    std::vector<std::byte> mipEvidence;
    {auto a=panel(material);PolygonImage image{"checker","image/raw",64,64,{}};
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){const auto v=static_cast<std::uint8_t>((x+y)%2?255:0);image.rgba8.insert(image.rgba8.end(),{v,v,v,v});}
        a.images={image};a.textures={{"checker",0,{}}};auto& b=a.materialBindings[0];b.baseColor.texture=0;b.metallicRoughness.texture=0;
        for(auto& v:a.vertices){v.texcoord.x*=128;v.texcoord.y*=128;}
        mipEvidence=render(a,"color");probe(mipEvidence,{.503F,.503F,.503F,128.F/255},"Full typed mip chain");
        probe(render(a,"channels"),{128.F/255,128.F/255,128.F/255,1},"Linear alpha mip average");
        // World projection footprint must ignore huge unused UV derivatives.
        b.mapping.mappingMode=MaterialMappingMode::WorldTriplanar;b.mapping.triplanarScale=1;
        a.images[0].rgba8=split.images[0].rgba8;a.images[0].width=2;a.images[0].height=1;
        for(auto& v:a.vertices){v.texcoord.x*=1e4F;v.texcoord.y*=1e4F;}
        b.baseColor.colorSpace=PolygonTextureColorSpace::Linear;b.mapping.baseTransform.offset={.25F,.5F};
        probe(render(a,"color"),{64.F/255,64.F/255,64.F/255,64.F/255},"Triplanar position derivatives",.015F);}
    {auto a=panel(material);a.images={{"LOD stripes","image/raw",4,1,{0,0,0,255,255,255,255,255,0,0,0,255,255,255,255,255}}};
        a.textures={{"stripes",0,{}}};a.materialBindings[0].baseColor={0,0,PolygonTextureColorSpace::Linear};
        for(auto& v:a.vertices)v.texcoord={.125F+(v.texcoord.x-32.5F/64)*64*std::sqrt(2.F)/4,.5F};
        probe(render(a,"color"),{.251F,.251F,.251F,1},"Fractional trilinear LOD",.015F);}
    {auto a=panel(material);PolygonImage n{"normal checker","image/raw",64,64,{}};
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)n.rgba8.insert(n.rgba8.end(),{static_cast<std::uint8_t>(x%2?218:37),128,218,255});
        a.images={n};a.textures={{"normals",0,{}}};a.materialBindings[0].normal.texture=0;
        for(auto& v:a.vertices){v.texcoord.x*=128;v.texcoord.y*=128;}
        probe(render(a,"normal"),{.5F,.5F,1,1},"Normalized GPU normal mips");}
    for(auto mapping:{MaterialMappingMode::WorldTriplanar,MaterialMappingMode::ObjectTriplanar})for(float strength:{0.F,1.F}){
        auto a=panel(material);const auto N=normalize(Float3{.3F,.4F,.8660254F});for(auto& v:a.vertices)v.normal=N;
        a.images={{"neutral normal","image/raw",1,1,{128,128,255,255}}};a.textures={{"normal",0,{}}};
        auto& b=a.materialBindings[0];b.normal.texture=0;b.normalScale=strength;b.mapping.mappingMode=mapping;b.mapping.triplanarBlendSharpness=8;
        probe(render(a,"normal"),{N.x*.5F+.5F,N.y*.5F+.5F,N.z*.5F+.5F,1},"Neutral triplanar normal");}
    // The actual live fragment shader must evaluate source maps, masks, RNM and height
    // rather than blending uniform material records before texture sampling.
    unsigned layerProbes=0;float layerWorst=0;
    for(const auto& mode:{std::string("color"),std::string("channels"),std::string("normal"),std::string("emission")})
    for(unsigned count:{0U,1U,2U,4U})for(unsigned mappingMode=0;mappingMode<4;++mappingMode){
        auto a=panel(material);a.materials[0].baseColor={.25F,.35F,.45F,1};a.materials[0].metallic=.8F;a.materials[0].roughness=.6F;a.materials[0].emissive={.02F,.03F,.04F};
        a.images={{"layer color","image/raw",2,2,{32,128,200,255,200,64,32,255,80,170,64,255,190,90,180,255}},
                  {"MR","image/raw",2,1,{255,80,200,255,255,200,20,255}},
                  {"normal","image/raw",2,1,{150,128,250,255,100,145,245,255}},
                  {"mask","image/raw",2,1,{40,40,40,255,230,230,230,255}}};
        a.samplers={{ImportedWrapMode::MirroredRepeat,ImportedWrapMode::Repeat,ImportedTextureFilter::Linear,ImportedTextureFilter::Linear}};
        for(unsigned j=0;j<a.images.size();++j)a.textures.push_back({"layer",j,0});
        for(unsigned j=0;j<count;++j){VoxelMaterialDefinition source;source.baseColor={.8F,.6F,.5F,.7F};source.metallic=.25F;source.roughness=.4F;source.emissive={.01F,.02F,.03F};if(mappingMode==3&&j==0){source.surfaceDetail.pattern=SurfacePattern::Paint;source.surfaceDetail.periodMeters=.013F;}
            a.materials.push_back(source);a.materialBindings.emplace_back();a.materials[0].layers.push_back({MaterialId(j+1),.2F+.1F*float(j),MaterialLayerBlendMode(j%3),true});
            auto& b=a.materialBindings.back();b.baseColor.texture=0;b.baseColor.texcoord=1;b.metallicRoughness.texture=1;b.metallicRoughness.texcoord=0;b.normal.texture=2;b.normal.texcoord=j%2;
            b.mapping.mappingMode=MaterialMappingMode(mappingMode);b.mapping.triplanarScale=35;b.mapping.baseTransform.rotationRadians=.2F;
            PolygonMaterialLayerBinding lb;lb.mask.texture=3;lb.mask.texcoord=1;lb.heightBlendStrength=j==0?0:.2F;lb.heightBlendBias=.01F;lb.opacityPolicy=MaterialLayerOpacityPolicy::Lerp;
            a.materialBindings[0].layers.push_back(lb);
        }
        a.contentHash=polygon_asset_content_hash(a);
        std::vector<PolygonTextureMipChain> mips;for(const auto& image:a.images){auto chain=generate_texture_mips(image);chain.normalLevels=generate_texture_mips(image,true).levels;chain.srgbLevels=generate_texture_mips(image,false,true).levels;mips.push_back(std::move(chain));}
        {
            const auto begin=std::chrono::steady_clock::now();const auto pixels=render(a,mode);
            if(mappingMode==0&&mode=="color"){auto payload=build_layered_surface_payload(a,0,&error);const auto cold=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();const auto warmBegin=std::chrono::steady_clock::now();(void)render(a,mode);std::cout<<count<<" overlays: "<<payload->words.size()*4<<" payload bytes; first "<<cold<<" ms, warm "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-warmBegin).count()<<" ms (software Vulkan, upload/readback included)\n";}
            for(unsigned y=7;y<width-5;y+=13)for(unsigned x=6;x<width-5;x+=13){
                PolygonBvhHit hit;hit.position={(2*(float(x)+.5F)/width-1)*.012F,(2*(float(y)+.5F)/width-1)*.012F,.2F};hit.normal={0,0,1};hit.firstIndex=hit.position.y<=hit.position.x?0:3;
                const auto expected=sample_polygon_hit_material(a,hit,{},hit.normal,mips,1,.024F/width);const auto& m=expected.material;
                const std::array<float,4> want=mode=="color"?std::array<float,4>{m.baseColor.x,m.baseColor.y,m.baseColor.z,m.roughness}:mode=="normal"?std::array<float,4>{expected.normal.x*.5F+.5F,expected.normal.y*.5F+.5F,expected.normal.z*.5F+.5F,m.roughness}:mode=="emission"?std::array<float,4>{m.emissive.x,m.emissive.y,m.emissive.z,m.baseColor.w}:std::array<float,4>{m.metallic,m.roughness,m.baseColor.w,1};
                for(unsigned c=0;c<4;++c){const auto actual=std::to_integer<unsigned>(pixels[(y*width+x)*4+c])/255.F;const auto delta=std::abs(actual-want[c]);layerWorst=(std::max)(layerWorst,delta);check(delta<.012F,"Layer CPU/GPU mismatch "+std::to_string(count)+" overlays mapping "+std::to_string(mappingMode)+" "+mode+" channel "+std::to_string(c)+" got "+std::to_string(actual)+" want "+std::to_string(want[c]));++layerProbes;}
            }
        }
    }
    std::cout<<layerProbes<<" layered CPU/GPU channel probes; max error "<<layerWorst<<'\n';
    for(auto policy:{MaterialLayerOpacityPolicy::Lerp,MaterialLayerOpacityPolicy::Multiply,MaterialLayerOpacityPolicy::Replace}){
        auto a=panel(material);a.materials[0].blendMode=MaterialBlendMode::Masked;
        VoxelMaterialDefinition source;source.baseColor={.7F,.4F,.2F,1};a.materials.push_back(source);a.materialBindings.emplace_back();a.materials[0].layers={{1,1,MaterialLayerBlendMode::Lerp,true}};
        a.images={{"layer cutout","image/raw",2,1,{0,0,0,255,255,255,255,255}}};a.textures={{"layer opacity",0,0}};
        a.samplers={{ImportedWrapMode::Repeat,ImportedWrapMode::Repeat,ImportedTextureFilter::Nearest,ImportedTextureFilter::Nearest}};
        a.materialBindings[1].opacity.texture=0;a.materialBindings[1].opacity.texcoord=1;
        PolygonMaterialLayerBinding lb;lb.heightBlendStrength=0;lb.opacityPolicy=policy;a.materialBindings[0].layers={lb};
        render(a,"color",true);
    }
    // Compare actual main-pass discard and dynamic atlas depth for distinct UVs,
    // transformed UVs, and world/object triplanar projection. No beauty-image proxy.
    for(unsigned mode=0;mode<5;++mode){auto a=panel(material);a.materials[0].blendMode=MaterialBlendMode::Masked;
        a.images={{"cutout","image/raw",2,1,{255,255,255,0,255,255,255,255}},
                  {"opacity","image/raw",2,1,{0,0,0,255,255,255,255,255}}};
        a.samplers={{ImportedWrapMode::Repeat,ImportedWrapMode::Repeat,ImportedTextureFilter::Nearest,ImportedTextureFilter::Nearest}};
        a.textures={{"cutout",0,0},{"opacity",1,0}};auto& b=a.materialBindings[0];
        b.baseColor.texture=0;b.baseColor.texcoord=1;b.opacity.texture=1;b.opacity.texcoord=0;
        for(auto& v:a.vertices){v.texcoord={.75F,.5F};v.texcoord1={(v.position.x+.012F)/.024F,(v.position.y+.012F)/.024F};}
        if(mode==1){b.mapping.baseTransform.scale={-1,1};b.mapping.baseTransform.offset={.2F,0};b.opacity.texture.reset();}
        if(mode==4){b.baseColor.texture.reset();b.opacity.texcoord=1;}
        if(mode==2||mode==3){b.mapping.mappingMode=mode==2?MaterialMappingMode::WorldTriplanar:MaterialMappingMode::ObjectTriplanar;b.mapping.triplanarScale=1/.024F;b.opacity.texture.reset();}
        render(a,"color",true);}
    if(!output.empty()){
        std::filesystem::create_directories(output);
        for(unsigned kind=0;kind<3;++kind){std::ofstream ppm(output/(kind==0?"texture-mip.gpu.ppm":kind==1?"texture-mip.reference.ppm":"texture-mip.diff.ppm"),std::ios::binary);ppm<<"P6\n64 64\n255\n";
            for(unsigned i=0;i<width*width;++i)for(unsigned c=0;c<3;++c){const unsigned actual=std::to_integer<unsigned>(mipEvidence[i*4+c]);const unsigned char value=static_cast<unsigned char>(kind==0?actual:kind==1?128:std::abs(int(actual)-128)*16);ppm.write(reinterpret_cast<const char*>(&value),1);}}
    }
    std::cout<<cases<<" GPU texture/mapping/filter cases; "<<shadowPixels<<" main/shadow coverage comparisons\n";
    check(destroy_live_environment_renderer(d,renderer,&error)&&d.destroy_texture(target,&error)&&d.destroy_texture(depth,&error),"Texture cleanup");
    check(destroy_cascaded_shadow_atlas(d,atlas,&error)&&destroy_environment_lighting(d,lighting,&error),"Texture lighting cleanup");
}

}
int main(int argc,char** argv){try{
#ifndef DVE_SURFACE_SHADER_DIR
    if(argc<2){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error("DXC surface shaders required");return dve::testing::skip("GPU surface recipes test: configure DXC or pass a shader directory");}
    const std::filesystem::path root=argv[1];
#else
    const std::filesystem::path root=argc>1?argv[1]:DVE_SURFACE_SHADER_DIR;
#endif
    VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));return dve::testing::skip("GPU surface recipes test: Vulkan unavailable");}
    if(argc<=3||std::string(argv[3])!="--textures-only"){analytic_conformance(d,root);live_pixels(d,root,argc>2?argv[2]:std::filesystem::path{});}texture_pixels(d,root,argc>2?argv[2]:std::filesystem::path{});std::cout<<"GPU surface recipes passed\n";return dve::testing::exit_code(false);
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
