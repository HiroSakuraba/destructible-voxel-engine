#include "dve/gpu_material_surface_detail.hpp"
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
    for(unsigned pattern=0;pattern<=5;++pattern)for(unsigned seed:{1U,7U,65520U,0xffffffffU})
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
}
int main(int argc,char** argv){try{
#ifndef DVE_SURFACE_SHADER_DIR
    if(argc<2){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error("DXC surface shaders required");std::cout<<"SKIP: configure DXC or pass shader directory\n";return 0;}
    const std::filesystem::path root=argv[1];
#else
    const std::filesystem::path root=argc>1?argv[1]:DVE_SURFACE_SHADER_DIR;
#endif
    VulkanDevice d;if(d.status()!=DeviceStatus::Ready){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string(d.device_loss_reason()));std::cout<<"SKIP: Vulkan unavailable\n";return 0;}
    analytic_conformance(d,root);live_pixels(d,root,argc>2?argv[2]:std::filesystem::path{});std::cout<<"GPU surface recipes passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
