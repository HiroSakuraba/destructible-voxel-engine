#include "dve/render/dashr_shell.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;

void require(bool condition,const char* message){
    if(!condition)throw std::runtime_error(message);
}
bool close(float a,float b,float epsilon=1.0e-4F){
    return std::abs(a-b)<=epsilon;
}

CookedPolygonAsset make_triangle(){
    CookedPolygonAsset asset;
    asset.objectId=77U;
    VoxelMaterialDefinition material;
    material.name="dashr-shell";
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});
    asset.vertices={
        {{0,0,0},{0,0,1},{1,0,0,1},{0.1F,0.1F},{1,1,1,1},{0,0}},
        {{1,0,0},{0,0,1},{1,0,0,1},{0.9F,0.1F},{1,1,1,1},{0,0}},
        {{0,1,0},{0,0,1},{1,0,0,1},{0.1F,0.9F},{1,1,1,1},{0,0}},
    };
    asset.indices={0,1,2};
    asset.submeshes={{"triangle",0U,3U,0U}};
    asset.bounds={{0,0,0},{1,1,0}};
    asset.contentHash=polygon_asset_content_hash(asset);
    require(static_cast<bool>(validate_polygon_asset(asset)),"fixture invalid");
    return asset;
}

void test_shell_geometry(){
    const auto asset=make_triangle();
    DashrSurfaceSettings settings;
    settings.heightScale=0.2F;
    settings.heightReferencePlane=0.5F;
    settings.envelopePadding=0.03F;
    std::string error;
    const auto shell=build_dashr_shell_mesh(asset,settings,{},&error);
    require(shell.has_value(),error.c_str());
    require(shell->vertices.size()==6U,"one source triangle should create six prism vertices");
    require(shell->indices.size()==24U,"one source triangle should create eight shell triangles");
    require(shell->stats.emittedTriangles==8U,"shell emitted-triangle count wrong");
    require(shell->submeshes.size()==1U&&shell->submeshes[0].indexCount==24U,
            "shell submesh index range wrong");
    require(close(shell->stats.minimumExtrusion,-0.13F),
            "shell lower extrusion does not cover mapped height envelope");
    require(close(shell->stats.maximumExtrusion,0.13F),
            "shell upper extrusion does not cover mapped height envelope");

    std::vector<Float3> deformed{{0,0,0},{2,0,0},{0,1,0}};
    const auto stretched=build_dashr_shell_mesh(asset,settings,deformed,&error);
    require(stretched.has_value(),error.c_str());
    require(close(stretched->vertices[1].positionX,2.0F),
            "shell did not consume deformed positions");
}

void test_explicit_image_sampler_bindings(){
    rhi::NullDevice device;
    std::string error;

    rhi::TextureDesc td;
    td.width=4U;td.height=4U;
    td.format=rhi::TextureFormat::RGBA8Unorm;
    td.usage=rhi::TextureUsage::Sampled|rhi::TextureUsage::CopyDestination;
    td.initialState=rhi::ResourceState::ShaderRead;
    const auto texture=device.create_texture(td,&error);
    require(texture,error.c_str());
    rhi::TextureViewDesc vd;vd.texture=texture;
    const auto view=device.create_texture_view(vd,&error);
    require(view,error.c_str());
    rhi::SamplerDesc sd;
    const auto sampler=device.create_sampler(sd,&error);
    require(sampler,error.c_str());

    rhi::BindGroupLayoutDesc layout;
    layout.bindings={
        {0U,rhi::BindingType::SampledImage,rhi::ShaderStage::Fragment},
        {1U,rhi::BindingType::Sampler,rhi::ShaderStage::Fragment},
    };
    const auto layoutHandle=device.create_bind_group_layout(layout,&error);
    require(layoutHandle,error.c_str());

    rhi::BindGroupDesc group;
    group.layout=layoutHandle;
    group.entries={
        {0U,{},view,0U,0U,{}},
        {1U,{},{},0U,0U,sampler},
    };
    const auto groupHandle=device.create_bind_group(group,&error);
    require(groupHandle,error.c_str());

    rhi::BindGroupDesc invalidImage=group;
    invalidImage.entries[0].sampler=sampler;
    require(!device.create_bind_group(invalidImage,&error),
            "sampled-image binding accepted a sampler");

    rhi::BindGroupDesc invalidSampler=group;
    invalidSampler.entries[1].textureView=view;
    require(!device.create_bind_group(invalidSampler,&error),
            "sampler binding accepted a texture view");
}

void test_shell_render_contract(){
    rhi::NullDevice device;
    std::string error;
    const auto asset=make_triangle();

    DashrSurfaceMeshMirror surfaceMirror(device);
    require(surfaceMirror.upload(asset,{},nullptr,&error),error.c_str());
    DashrAtlasResources atlas;
    DashrAtlasShaderBytecode atlasBytecode{
        {std::byte{1}},{std::byte{2}},{std::byte{3}}};
    require(create_dashr_atlas_resources(device,atlasBytecode,64U,atlas,&error),error.c_str());
    DashrAtlasUpdateStats atlasStats;
    require(record_dashr_atlas_update(device,atlas,surfaceMirror,asset,atlasStats,nullptr,&error),
            error.c_str());
    require(atlas.published,"atlas was not published");

    DashrSurfaceSettings settings;
    settings.heightScale=0.12F;
    settings.envelopePadding=0.02F;
    const auto shell=build_dashr_shell_mesh(asset,settings,{},&error);
    require(shell.has_value(),error.c_str());
    DashrShellMeshMirror shellMirror(device);
    require(shellMirror.upload(*shell,&error),error.c_str());

    rhi::TextureDesc heightDesc;
    heightDesc.width=8U;heightDesc.height=8U;
    heightDesc.format=rhi::TextureFormat::RGBA8Unorm;
    heightDesc.usage=rhi::TextureUsage::Sampled|rhi::TextureUsage::CopyDestination;
    heightDesc.initialState=rhi::ResourceState::ShaderRead;
    const auto height=device.create_texture(heightDesc,&error);
    require(height,error.c_str());
    std::vector<std::byte> heightPixels(8U*8U*4U,std::byte{0x80});
    require(device.write_texture(height,0U,0U,heightPixels,8U*4U,&error),error.c_str());
    rhi::TextureViewDesc heightViewDesc;heightViewDesc.texture=height;
    const auto heightView=device.create_texture_view(heightViewDesc,&error);
    require(heightView,error.c_str());
    rhi::SamplerDesc heightSamplerDesc;
    const auto heightSampler=device.create_sampler(heightSamplerDesc,&error);
    require(heightSampler,error.c_str());

    DashrShellRendererResources renderer;
    DashrShellShaderBytecode shellBytecode{{std::byte{4}},{std::byte{5}}};
    require(create_dashr_shell_renderer(
        device,shellBytecode,rhi::TextureFormat::RGBA8Unorm,4096U,renderer,&error),
        error.c_str());

    rhi::TextureDesc color;
    color.width=96U;color.height=64U;color.format=rhi::TextureFormat::RGBA8Unorm;
    color.usage=rhi::TextureUsage::RenderTarget|rhi::TextureUsage::CopySource;
    color.initialState=rhi::ResourceState::RenderTarget;
    const auto colorTarget=device.create_texture(color,&error);
    require(colorTarget,error.c_str());
    rhi::TextureDesc depth=color;
    depth.format=rhi::TextureFormat::D32Float;
    depth.usage=rhi::TextureUsage::DepthStencil|rhi::TextureUsage::CopySource;
    depth.initialState=rhi::ResourceState::DepthWrite;
    const auto depthTarget=device.create_texture(depth,&error);
    require(depthTarget,error.c_str());

    DashrShellDraw draw;
    draw.shell=&shellMirror;
    draw.atlas=&atlas;
    draw.heightView=heightView;
    draw.heightSampler=heightSampler;
    draw.shellSubmeshIndex=0U;
    draw.cameraObjectPosition={0.25F,0.25F,2.0F};
    draw.settings=settings;
    draw.debugMode=2U;

    DashrShellFrameDesc frame;
    frame.colorTarget=colorTarget;
    frame.depthTarget=depthTarget;
    frame.width=96U;frame.height=64U;
    frame.draws=std::span(&draw,1U);

    const auto before=device.statistics();
    DashrShellFrameStats stats;
    rhi::FenceHandle fence;
    require(record_dashr_shell_frame(device,renderer,frame,stats,&fence,&error),error.c_str());
    require(fence&&device.fence_complete(fence),"shell frame fence did not complete");
    require(stats.draws==1U&&stats.shellTriangles==8U,
            "shell frame draw statistics are wrong");
    require(stats.transientBindGroups==2U&&stats.constantRanges==1U,
            "shell descriptor/constant statistics are wrong");
    const auto after=device.statistics();
    require(after.renderPassesExecuted==before.renderPassesExecuted+1U,
            "shell frame did not execute one load-preserving pass");
    require(after.indexedDrawsExecuted==before.indexedDrawsExecuted+1U,
            "shell frame did not execute its indexed prism draw");

    require(destroy_dashr_shell_renderer(device,renderer,&error),error.c_str());
    require(destroy_dashr_atlas_resources(device,atlas,&error),error.c_str());
}

}

int main(){
    try{
        test_shell_geometry();
        test_explicit_image_sampler_bindings();
        test_shell_render_contract();
        std::cout<<"dve_dashr_shell_tests: PASS\n";
        return 0;
    }catch(const std::exception&e){
        std::cerr<<"dve_dashr_shell_tests: FAIL: "<<e.what()<<'\n';
        return 1;
    }
}
