#include "dve/hybrid_scene_manifest.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/render/mesh_heap.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(c) do { if(!(c)) throw std::runtime_error(std::string("CHECK failed: ")+#c+" at "+__FILE__+":"+std::to_string(__LINE__)); } while(false)
using namespace dve;

CookedPolygonAsset make_quad(std::uint64_t id=1, bool textured=true) {
    CookedPolygonAsset asset;
    asset.objectId=id;
    VoxelMaterialDefinition material;
    material.name="ClearCoatBlue";
    material.baseColor={0.08F,0.28F,0.85F,1.0F};
    material.roughness=0.25F;
    material.shadingModel=MaterialShadingModel::ClearCoat;
    material.clearCoat=0.8F;
    material.clearCoatRoughness=0.08F;
    asset.materials.push_back(material);
    PolygonMaterialBinding binding;
    binding.doubleSided=false;
    if(textured) binding.baseColor.texture=0U;
    asset.materialBindings.push_back(binding);
    if(textured) {
        PolygonImage image;
        image.name="checker";image.mimeType="image/raw";image.width=4;image.height=4;
        for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<4;++x){
            const bool bright=((x+y)&1U)==0U;
            image.rgba8.insert(image.rgba8.end(),{static_cast<std::uint8_t>(bright?255:80),
                                                   static_cast<std::uint8_t>(bright?255:110),
                                                   static_cast<std::uint8_t>(bright?255:220),255});
        }
        asset.images.push_back(std::move(image));asset.samplers.push_back({});asset.textures.push_back({"checker",0U,0U});
    }
    asset.vertices={
        {{-1,-1,0},{0,0,1},{1,0,0,1},{0,1},{1,1,1,1}},
        {{ 1,-1,0},{0,0,1},{1,0,0,1},{1,1},{1,1,1,1}},
        {{ 1, 1,0},{0,0,1},{1,0,0,1},{1,0},{1,1,1,1}},
        {{-1, 1,0},{0,0,1},{1,0,0,1},{0,0},{1,1,1,1}}};
    // Counter-clockwise in world space when viewed from +Z. The software screen transform flips Y.
    asset.indices={0,1,2,0,2,3};
    asset.submeshes.push_back({"quad",0,6,0});
    asset.bounds={{-1,-1,0},{1,1,0}};
    asset.contentHash=polygon_asset_content_hash(asset);
    CHECK(validate_polygon_asset(asset));
    return asset;
}

std::filesystem::path temp_file(std::string_view suffix) {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()/("dve_v133_"+std::to_string(stamp)+std::string(suffix));
}

void test_null_graphics_contract_and_texture_io() {
    rhi::NullDevice device;std::string error;
    rhi::TextureDesc colorDesc;colorDesc.width=64;colorDesc.height=48;colorDesc.usage=rhi::TextureUsage::RenderTarget|rhi::TextureUsage::CopySource|rhi::TextureUsage::CopyDestination;colorDesc.initialState=rhi::ResourceState::Undefined;
    rhi::TextureDesc depthDesc=colorDesc;depthDesc.format=rhi::TextureFormat::D32Float;depthDesc.usage=rhi::TextureUsage::DepthStencil|rhi::TextureUsage::CopySource;depthDesc.initialState=rhi::ResourceState::Undefined;
    const auto color=device.create_texture(colorDesc,&error),depth=device.create_texture(depthDesc,&error);CHECK(color&&depth);
    rhi::TextureDesc sampled;colorDesc.width=4;colorDesc.height=4;sampled=colorDesc;sampled.width=4;sampled.height=4;sampled.mipLevels=3;sampled.usage=rhi::TextureUsage::Sampled|rhi::TextureUsage::CopyDestination|rhi::TextureUsage::CopySource;
    const auto texture=device.create_texture(sampled,&error);CHECK(texture);
    std::array<std::byte,64> pixels{};for(std::size_t i=0;i<pixels.size();++i)pixels[i]=static_cast<std::byte>(i);
    CHECK(device.write_texture(texture,0,0,pixels,16,&error));std::array<std::byte,64> read{};CHECK(device.read_texture(texture,0,0,read,16,&error));CHECK(read==pixels);
    rhi::BufferDesc vbDesc{4U*16U,rhi::BufferUsage::Vertex|rhi::BufferUsage::CopyDestination,rhi::MemoryDomain::DeviceLocal,"vb"};
    rhi::BufferDesc ibDesc{6U*4U,rhi::BufferUsage::Index|rhi::BufferUsage::CopyDestination,rhi::MemoryDomain::DeviceLocal,"ib"};
    const auto vb=device.create_buffer(vbDesc,&error),ib=device.create_buffer(ibDesc,&error);CHECK(vb&&ib);
    rhi::GraphicsPipelineDesc pipelineDesc;pipelineDesc.vertexBytecode={std::byte{1}};pipelineDesc.fragmentBytecode={std::byte{2}};
    const auto pipeline=device.create_graphics_pipeline(pipelineDesc,&error);CHECK(pipeline);
    const auto commands=device.begin_commands(rhi::QueueKind::Graphics,"polygon",&error);CHECK(commands);
    CHECK(!device.draw_indexed(commands, 6, 1, 0, 0, 0, &error));
    error.clear();
    CHECK(device.transition_texture(commands,color,rhi::ResourceState::Undefined,rhi::ResourceState::RenderTarget,&error));
    CHECK(device.transition_texture(commands,depth,rhi::ResourceState::Undefined,rhi::ResourceState::DepthWrite,&error));
    rhi::RenderPassDesc pass;pass.colors.push_back({color,true,0.1F,0.2F,0.3F,1.0F});pass.depth=rhi::RenderPassDepthAttachment{depth,true,1.0F};
    CHECK(device.begin_render_pass(commands,pass,&error));CHECK(device.bind_graphics_pipeline(commands,pipeline,&error));
    CHECK(device.set_viewport(commands,{0,0,64,48,0,1},&error));CHECK(device.set_scissor(commands,{0,0,64,48},&error));
    CHECK(device.bind_vertex_buffer(commands,0,vb,0,16,&error));CHECK(device.bind_index_buffer(commands,ib,0,rhi::IndexFormat::Uint32,&error));
    CHECK(device.draw_indexed(commands,6,3,0,0,0,&error));CHECK(device.end_render_pass(commands,&error));
    CHECK(device.transition_texture(commands,color,rhi::ResourceState::RenderTarget,rhi::ResourceState::ShaderRead,&error));
    const auto fence=device.submit(commands,&error);CHECK(fence);const auto stats=device.statistics();CHECK(stats.renderPassesExecuted==1U);CHECK(stats.indexedDrawsExecuted==1U);CHECK(stats.indexedTrianglesSubmitted==6U);
    CHECK(device.destroy_graphics_pipeline(pipeline,&error));CHECK(device.destroy_buffer(vb,&error));CHECK(device.destroy_buffer(ib,&error));CHECK(device.destroy_texture(texture,&error));CHECK(device.destroy_texture(color,&error));CHECK(device.destroy_texture(depth,&error));
}

void test_reference_renderer_mips_lod_and_hybrid_depth() {
    CookedPolygonAsset asset=make_quad(10,true);const auto mips=render::generate_texture_mips(asset.images[0]);CHECK(mips.levels.size()==3U);CHECK(mips.levels[1].width==2U);CHECK(mips.levels[2].width==1U);
    render::PolygonRenderTarget target;target.resize(256,192);target.clear();render::PolygonCamera camera;camera.position={0,0,3};camera.target={0,0,0};
    RenderEnvironment environment;environment.sunDirection={0.4F,0.5F,1.0F};environment.sunIntensity=2.0F;
    render::PolygonRenderInstance instance{10,&asset,{}, {}, {1,1,1,1},true};render::ReferencePolygonRenderer renderer;const auto stats=renderer.render(std::span(&instance,1),camera,environment,target,{.preserveExistingDepth=false});
    CHECK(stats.submittedTriangles==2U);CHECK(stats.rasterizedTriangles==2U);CHECK(stats.shadedFragments>1000U);
    CHECK(stats.textureSamples>1000U);
    const std::size_t center=static_cast<std::size_t>(target.height/2U)*target.width+target.width/2U;CHECK(target.depth[center]<1.0F);CHECK(target.objectId[center]==10U);CHECK(target.hdrColor[center].z>target.hdrColor[center].x);

    CookedPolygonAsset coarse = make_quad(11, false);
    coarse.materials[0].baseColor = {0.85F, 0.08F, 0.04F, 1.0F};
    coarse.contentHash = polygon_asset_content_hash(coarse);
    render::PolygonRenderTarget lodTarget;
    lodTarget.resize(256, 192);
    const std::array<render::PolygonLodLevel, 1> lods{{{&coarse, 10.0F}}};
    render::PolygonRenderInstance lodInstance{12, &asset, {}, lods, {1,1,1,1}, true};
    const auto lodStats = renderer.render(std::span(&lodInstance, 1), camera, environment, lodTarget, {});
    CHECK(lodStats.lodSelections == 1U);
    CHECK(lodTarget.objectId[center] == 12U);
    CHECK(lodTarget.hdrColor[center].x > lodTarget.hdrColor[center].z);

    render::PolygonRenderTarget voxel;voxel.resize(256,192);voxel.clear({0.8F,0.1F,0.1F,1},0.001F);voxel.objectId.assign(voxel.objectId.size(),99U);
    render::PolygonRenderTarget composite;std::string error;CHECK(render::composite_hybrid_layers(voxel,target,composite,&error));CHECK(composite.objectId[center]==99U);voxel.depth[center]=0.9F;CHECK(render::composite_hybrid_layers(voxel,target,composite,&error));CHECK(composite.objectId[center]==10U);
    const auto path=temp_file(".ppm");CHECK(render::write_polygon_render_ppm(path,target,1.0F,&error));CHECK(std::filesystem::file_size(path)>1000U);std::filesystem::remove(path);
}


void test_full_material_texture_channels() {
    CookedPolygonAsset asset = make_quad(20, false);
    asset.materials[0].baseColor = {0.45F,0.45F,0.45F,1.0F};
    asset.materials[0].metallic = 0.8F;
    asset.materials[0].roughness = 0.8F;
    asset.materials[0].emissive = {1.0F,1.0F,1.0F};
    const auto add_texture = [&](std::string name, std::array<std::uint8_t,4> rgba) {
        PolygonImage image; image.name=name; image.mimeType="image/raw"; image.width=1; image.height=1;
        image.rgba8.assign(rgba.begin(),rgba.end());
        asset.images.push_back(image); asset.samplers.push_back({});
        asset.textures.push_back({name,static_cast<std::uint32_t>(asset.images.size()-1U),
                                  static_cast<std::uint32_t>(asset.samplers.size()-1U)});
        return static_cast<std::uint32_t>(asset.textures.size()-1U);
    };
    auto& binding=asset.materialBindings[0];
    binding.baseColor.texture=add_texture("base",{190,150,110,255});
    binding.metallicRoughness.texture=add_texture("mr",{0,64,200,255});
    binding.metallicRoughness.texcoord=1U;
    binding.normal.texture=add_texture("normal",{210,128,220,255});
    binding.normalScale=0.8F;
    binding.emissive.texture=add_texture("emissive",{120,20,10,255});
    binding.emissive.texcoord=1U;
    binding.opacity.texture=add_texture("opacity",{255,255,255,255});
    binding.opacity.texcoord=1U;
    for(auto& vertex:asset.vertices) vertex.texcoord1=vertex.texcoord;
    asset.contentHash=polygon_asset_content_hash(asset); CHECK(validate_polygon_asset(asset));

    render::PolygonCamera camera; camera.position={0,0,3}; camera.target={0,0,0};
    RenderEnvironment environment; environment.sunDirection={0.2F,0.3F,1.0F}; environment.sunIntensity=1.5F;
    render::ReferencePolygonRenderer renderer;
    render::PolygonRenderTarget mapped; mapped.resize(160,120);
    render::PolygonRenderInstance instance{20,&asset,{}, {}, {1,1,1,1},true};
    const auto mappedStats=renderer.render(std::span(&instance,1),camera,environment,mapped,{});
    const std::size_t center=static_cast<std::size_t>(mapped.height/2U)*mapped.width+mapped.width/2U;
    CHECK(mappedStats.textureSamples>=5U);
    CHECK(mapped.objectId[center]==20U);

    CookedPolygonAsset noEmissive=asset;
    noEmissive.materialBindings[0].emissive.texture.reset();
    noEmissive.materials[0].emissive={0.0F,0.0F,0.0F};
    noEmissive.contentHash=polygon_asset_content_hash(noEmissive);
    render::PolygonRenderTarget dark; dark.resize(160,120);
    render::PolygonRenderInstance darkInstance{21,&noEmissive,{}, {}, {1,1,1,1},true};
    const auto darkStats=renderer.render(std::span(&darkInstance,1),camera,environment,dark,{});
    CHECK(darkStats.shadedFragments>0U);
    CHECK(mapped.hdrColor[center].x>dark.hdrColor[center].x);

    CookedPolygonAsset rejected=asset;
    rejected.materials[0].blendMode=MaterialBlendMode::Masked;
    rejected.materialBindings[0].alphaCutoff=0.5F;
    const auto opacityTexture=*rejected.materialBindings[0].opacity.texture;
    const auto opacityImage=rejected.textures[opacityTexture].imageIndex;
    rejected.images[opacityImage].rgba8={0,0,0,255};
    rejected.contentHash=polygon_asset_content_hash(rejected);
    render::PolygonRenderTarget maskTarget; maskTarget.resize(160,120);
    render::PolygonRenderInstance maskInstance{22,&rejected,{}, {}, {1,1,1,1},true};
    const auto maskStats=renderer.render(std::span(&maskInstance,1),camera,environment,maskTarget,{});
    CHECK(maskStats.alphaRejectedFragments>0U);
    CHECK(maskTarget.objectId[center]==0U);
}

void test_material_debug_views() {
    CookedPolygonAsset asset = make_quad(30, false);
    asset.materials[0].baseColor = {0.2F, 0.4F, 0.6F, 0.35F};
    asset.materials[0].metallic = 0.7F;
    asset.materials[0].roughness = 0.3F;
    asset.materials[0].emissive = {0.1F, 0.2F, 0.3F};
    asset.materials[0].blendMode = MaterialBlendMode::Translucent;
    asset.materials[0].transparent = true;
    asset.materialBindings[0].mapping.mappingMode = MaterialMappingMode::UV1;
    asset.materialBindings[0].mapping.detailFadeStartMeters = 0.0F;
    asset.materialBindings[0].mapping.detailFadeEndMeters = 10.0F;
    asset.vertices[0].texcoord1 = {0.0F, 1.0F};
    asset.vertices[1].texcoord1 = {1.0F, 1.0F};
    asset.vertices[2].texcoord1 = {1.0F, 0.0F};
    asset.vertices[3].texcoord1 = {0.0F, 0.0F};
    asset.contentHash = polygon_asset_content_hash(asset);
    CHECK(validate_polygon_asset(asset));

    render::PolygonRenderTarget target;
    target.resize(128U, 96U);
    render::PolygonCamera camera;
    camera.position = {0.0F, 0.0F, 3.0F};
    camera.target = {0.0F, 0.0F, 0.0F};
    RenderEnvironment environment;
    render::PolygonRenderInstance instance{30U, &asset, {}, {}, {1,1,1,1}, true};
    render::ReferencePolygonRenderer renderer;
    const std::size_t center = static_cast<std::size_t>(target.height / 2U) * target.width +
                               target.width / 2U;

    render::PolygonRenderOptions options;
    options.preserveExistingDepth = false;
    options.materialDebugView = render::PolygonMaterialDebugView::Roughness;
    CHECK(renderer.render(std::span(&instance, 1), camera, environment, target, options).shadedFragments > 0U);
    CHECK(std::abs(target.hdrColor[center].x - 0.3F) < 0.02F);
    CHECK(target.objectId[center] == 30U); // Debug views bypass translucent composition.

    options.materialDebugView = render::PolygonMaterialDebugView::WorldNormal;
    CHECK(renderer.render(std::span(&instance, 1), camera, environment, target, options).shadedFragments > 0U);
    CHECK(target.hdrColor[center].z > 0.95F);
    CHECK(std::abs(target.hdrColor[center].x - 0.5F) < 0.02F);

    options.materialDebugView = render::PolygonMaterialDebugView::Uv1;
    CHECK(renderer.render(std::span(&instance, 1), camera, environment, target, options).shadedFragments > 0U);
    CHECK(target.hdrColor[center].x > 0.45F && target.hdrColor[center].x < 0.55F);
    CHECK(target.hdrColor[center].y > 0.45F && target.hdrColor[center].y < 0.55F);

    options.materialDebugView = render::PolygonMaterialDebugView::DetailFade;
    CHECK(renderer.render(std::span(&instance, 1), camera, environment, target, options).shadedFragments > 0U);
    CHECK(target.hdrColor[center].x > 0.7F && target.hdrColor[center].x < 0.9F);
}


void test_per_pixel_layers_and_decals() {
    CookedPolygonAsset asset=make_quad(40,false);
    asset.materials[0].name="Painted metal";asset.materials[0].baseColor={0.1F,0.1F,0.1F,1.0F};
    asset.materials[0].metallic=1.0F;asset.materials[0].roughness=0.6F;
    VoxelMaterialDefinition paint;paint.name="Paint";paint.baseColor={0.8F,0.05F,0.02F,1.0F};paint.metallic=0.0F;paint.roughness=0.25F;
    asset.materials.push_back(paint);asset.materialBindings.push_back({});
    PolygonImage mask;mask.name="paint mask";mask.mimeType="image/raw";mask.width=1;mask.height=1;mask.rgba8={255,255,255,255};
    asset.images.push_back(mask);asset.samplers.push_back({});asset.textures.push_back({"paint mask",0U,0U});
    asset.materials[0].layers.push_back({1U,1.0F,MaterialLayerBlendMode::Lerp,true});
    PolygonMaterialLayerBinding layerBinding;layerBinding.semantic=MaterialLayerSemantic::Paint;layerBinding.mask.texture=0U;layerBinding.heightBlendStrength=0.0F;
    asset.materialBindings[0].layers.push_back(layerBinding);asset.contentHash=polygon_asset_content_hash(asset);CHECK(validate_polygon_asset(asset));

    render::PolygonCamera camera;camera.position={0,0,3};camera.target={0,0,0};RenderEnvironment environment;
    render::PolygonRenderTarget target;target.resize(128,96);render::PolygonRenderInstance instance{40,&asset,{}, {}, {1,1,1,1},true};render::ReferencePolygonRenderer renderer;
    render::PolygonRenderOptions options;options.preserveExistingDepth=false;options.materialDebugView=render::PolygonMaterialDebugView::LayerCoverage;
    auto stats=renderer.render(std::span(&instance,1),camera,environment,target,options);
    const std::size_t center=static_cast<std::size_t>(target.height/2U)*target.width+target.width/2U;
    CHECK(stats.layeredFragments>0U&&stats.layerMaskSamples>0U&&target.hdrColor[center].x>0.95F);
    options.materialDebugView=render::PolygonMaterialDebugView::BaseColor;stats=renderer.render(std::span(&instance,1),camera,environment,target,options);
    CHECK(target.hdrColor[center].x>0.75F&&target.hdrColor[center].y<0.1F);

    MaterialDecal wet;wet.id=1U;wet.semantic=MaterialDecalSemantic::Wetness;wet.channels=MaterialDecalChannel::Roughness;
    wet.layer.surface.roughness=0.02F;wet.layer.mask=1.0F;wet.layer.heightBlendStrength=0.0F;
    options.materialDebugView=render::PolygonMaterialDebugView::Roughness;options.decals=std::span<const MaterialDecal>(&wet,1);
    stats=renderer.render(std::span(&instance,1),camera,environment,target,options);
    CHECK(stats.decalFragments>0U&&target.hdrColor[center].x<0.05F);
}

void test_mesh_heap_bvh_and_manifest() {
    CookedPolygonAsset a=make_quad(1,false),b=make_quad(2,false);b.materials[0].baseColor={0.8F,0.2F,0.1F,1};b.contentHash=polygon_asset_content_hash(b);
    rhi::NullDevice device;render::ImmutableMeshHeap heap(device);std::string error;const std::array<const CookedPolygonAsset*,3> assets{&a,&a,&b};CHECK(heap.rebuild(assets,&error));CHECK(heap.stats().sourceAssets==3U);CHECK(heap.stats().uniqueAssets==2U);CHECK(heap.stats().deduplicatedAssets==1U);
    const std::array<render::MeshHeapInstance,3> instances{{{1,0,make_rigid_transform({0,0,0},{}),0,0},{2,0,make_rigid_transform({2,0,0},{}),0,0},{3,1,make_rigid_transform({-2,0,0},{}),0,0}}};CHECK(heap.upload_instances(instances,&error));CHECK(heap.readback_matches(&error));
    PolygonBvh bvh;CHECK(bvh.build(a,&error));CHECK(bvh.stats().triangles==2U);const auto hit=bvh.raycast({0,0,2},{0,0,-1},10);CHECK(hit);CHECK(std::abs(hit->distance-2.0F)<1.0e-4F);CHECK(bvh.sphere_overlap({0,0,0},0.1F));CHECK(!bvh.sphere_overlap({5,5,5},0.1F));
    HybridSceneManifest manifest;manifest.name="Hybrid test";manifest.requiredMode=GeometryBuildMode::Hybrid;manifest.assets.push_back({1,"wall",GeometryKind::Polygon,"wall.dmesh",a.contentHash,{},false,true,{"wall_albedo.png"}});manifest.assets.push_back({2,"rubble",GeometryKind::Voxel,"rubble.dvox",1234,make_rigid_transform({1,0,0},{}),true,true,{}});manifest.contentHash=hybrid_scene_manifest_hash(manifest);CHECK(validate_hybrid_scene_manifest(manifest,&error));CHECK(manifest_compatible_with_build(manifest,GeometryBuildMode::Hybrid,&error));CHECK(!manifest_compatible_with_build(manifest,GeometryBuildMode::VoxelOnly,&error));const auto path=temp_file(".dvescene");CHECK(write_dvescene(path,manifest,&error));const auto read=read_dvescene(path);CHECK(read);CHECK(read.manifest.assets.size()==2U);CHECK(read.manifest.contentHash==manifest.contentHash);std::filesystem::remove(path);
}
}

int main(){try{test_null_graphics_contract_and_texture_io();test_reference_renderer_mips_lod_and_hybrid_depth();test_full_material_texture_channels();test_material_debug_views();test_per_pixel_layers_and_decals();test_mesh_heap_bvh_and_manifest();std::cout<<"polygon rendering tests: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
