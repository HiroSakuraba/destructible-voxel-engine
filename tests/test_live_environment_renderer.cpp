#include "dve/environment_lighting.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/render/live_environment_renderer.hpp"
#include "dve/render/mesh_rhi_mirror.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)

dve::CookedPolygonAsset make_triangle() {
    dve::CookedPolygonAsset asset; asset.objectId = 66U;
    dve::VoxelMaterialDefinition material; material.name = "IBL metal"; material.baseColor = {0.5F,0.6F,0.8F,0.25F}; material.blendMode = dve::MaterialBlendMode::Masked; material.metallic = 0.7F; material.roughness = 0.25F;
    asset.materials.push_back(material); asset.materialBindings.push_back({});
    asset.images.push_back({"base alpha", "image/raw", 1U, 1U, {255U,255U,255U,192U}});
    asset.images.push_back({"opacity", "image/raw", 1U, 1U, {128U,128U,128U,255U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"base alpha", 0U, 0U});
    asset.textures.push_back({"opacity", 1U, 0U});
    asset.materialBindings[0].baseColor.texture = 0U;
    asset.materialBindings[0].opacity.texture = 1U;
    asset.vertices = {{{-0.8F,-0.8F,0.0F},{0,0,1},{1,0,0,1},{0,1},{1,1,1,1}},
                      {{ 0.8F,-0.8F,0.0F},{0,0,1},{1,0,0,1},{1,1},{1,1,1,1}},
                      {{ 0.0F, 0.8F,0.0F},{0,0,1},{1,0,0,1},{0.5F,0},{1,1,1,1}}};
    asset.indices = {0U,1U,2U}; asset.submeshes.push_back({"triangle",0U,3U,0U});
    asset.bounds = {{-0.8F,-0.8F,0.0F},{0.8F,0.8F,0.0F}}; asset.contentHash = dve::polygon_asset_content_hash(asset);
    CHECK(dve::validate_polygon_asset(asset)); return asset;
}
}

int main() {
    try {
        dve::rhi::NullDevice device; std::string error;
        dve::IblBakeSettings bake; bake.irradianceResolution=4U; bake.specularResolution=8U; bake.specularMipLevels=4U; bake.sampleCount=16U; bake.brdfResolution=8U;
        const auto assetLighting = dve::make_default_environment_lighting_asset(bake);
        dve::render::EnvironmentLightingGpuResources lighting;
        CHECK(dve::render::upload_environment_lighting(device, assetLighting, lighting, &error));
        dve::camera::CameraPose camera; camera.position={0,3,8}; camera.target={0,0,0}; camera.lens.farPlaneMeters=200.0F; camera.lens.aspectRatio=16.0F/9.0F;
        dve::render::CascadedShadowSettings settings; settings.cascadeResolution=64U; settings.maximumDistanceMeters=80.0F;
        const auto shadowPlan = dve::render::make_cascaded_shadow_plan(camera,{0.4F,0.8F,0.2F},settings);
        dve::render::CascadedShadowAtlasResources atlas; CHECK(dve::render::create_cascaded_shadow_atlas(device,shadowPlan,atlas,&error));
        CHECK(atlas.depthAtlas);

        const auto triangle = make_triangle(); dve::render::MeshRhiMirror mirror(device); CHECK(mirror.upload(triangle,&error));
        dve::render::LiveEnvironmentShaderBytecode bytecode;
        bytecode.skyboxVertex={std::byte{1}}; bytecode.skyboxFragment={std::byte{2}};
        bytecode.materialVertex={std::byte{3}}; bytecode.materialFragment={std::byte{4}};
        bytecode.shadowVertex={std::byte{5}}; bytecode.shadowFragment={std::byte{6}};
        dve::render::LiveEnvironmentRendererResources renderer;
        CHECK(dve::render::create_live_environment_renderer(device,lighting,atlas,bytecode,256U,dve::rhi::TextureFormat::RGBA8Unorm,renderer,&error));

        dve::rhi::TextureDesc color; color.width=96U;color.height=64U;color.usage=dve::rhi::TextureUsage::RenderTarget|dve::rhi::TextureUsage::CopySource;color.initialState=dve::rhi::ResourceState::RenderTarget;color.debugName="live environment color";
        dve::rhi::TextureDesc depth=color; depth.format=dve::rhi::TextureFormat::D32Float;depth.usage=dve::rhi::TextureUsage::DepthStencil|dve::rhi::TextureUsage::CopySource;depth.initialState=dve::rhi::ResourceState::DepthWrite;depth.debugName="live environment depth";
        const auto colorTarget=device.create_texture(color,&error); const auto depthTarget=device.create_texture(depth,&error); CHECK(colorTarget&&depthTarget);
        std::array<std::byte,256> constants{}; const dve::render::LivePolygonDraw draw{&mirror,&triangle,1U};
        dve::render::LiveEnvironmentFrameDesc frame; frame.colorTarget=colorTarget;frame.depthTarget=depthTarget;frame.width=96U;frame.height=64U;frame.frameConstants=constants;frame.polygonDraws=std::span(&draw,1U);
        dve::render::LiveEnvironmentFrameStats stats; dve::rhi::FenceHandle fence;
        CHECK(dve::render::record_live_environment_frame(device,lighting,shadowPlan,atlas,renderer,frame,stats,&fence,&error));
        CHECK(fence&&device.fence_complete(fence)); CHECK(stats.skyboxDraws==1U); CHECK(stats.materialDraws==1U); CHECK(stats.shadowDraws==4U); CHECK(stats.cascadesRendered==8U);
        CHECK(stats.staticCascadesRendered==4U&&stats.dynamicCascadesRendered==4U);
        CHECK(stats.materialTriangles==1U&&stats.shadowTriangles==4U); CHECK(stats.alphaMaskedShadowDraws==4U); CHECK(stats.objectConstantRanges==1U); CHECK(stats.cascadeConstantRanges==4U);
        CHECK(stats.mainMaterialDescriptorsBound==1U); CHECK(stats.gpuMaterialRecordsBound==1U); CHECK(stats.gpuMappingRecordsBound==1U);
        CHECK(stats.texturedAlphaShadowDraws==4U); CHECK(stats.baseColorAlphaShadowDraws==4U); CHECK(stats.opacityTextureShadowDraws==4U);
        CHECK(stats.alphaTextureFallbackDraws==0U);
        CHECK(&renderer.shadowMaterials->residency()==renderer.materialResidency.get());
        CHECK(&renderer.mainMaterials->residency()==renderer.materialResidency.get());
        auto tableStats=renderer.shadowMaterials->stats(); CHECK(tableStats.materialDescriptorCount==1U); CHECK(tableStats.imageResourceCount==2U); CHECK(tableStats.samplerResourceCount==1U);
        auto mainTableStats=renderer.mainMaterials->stats(); CHECK(mainTableStats.materialDescriptorCount==1U); CHECK(mainTableStats.assetResourceCount==1U); CHECK(mainTableStats.materialRecordsUploaded==1U); CHECK(mainTableStats.mappingRecordsUploaded==1U); CHECK(mainTableStats.imageResourceCount==2U);
        auto residencyStats=renderer.materialResidency->stats(); CHECK(residencyStats.imagesUploaded==2U); CHECK(residencyStats.samplersCreated==1U); CHECK(residencyStats.retainedAssetCount==1U); CHECK(residencyStats.assetReferenceCount==2U);
        const auto recorded=device.statistics(); CHECK(recorded.renderPassesExecuted==3U); CHECK(recorded.indexedDrawsExecuted==6U);

        auto staticDraw=draw; staticDraw.staticShadowCaster=true; frame.polygonDraws=std::span(&staticDraw,1U); frame.dirtyCascades={0U}; frame.refreshStaticShadowCasters=false;
        CHECK(dve::render::record_live_environment_frame(device,lighting,shadowPlan,atlas,renderer,frame,stats,&fence,&error));
        CHECK(stats.shadowDraws==0U); CHECK(stats.staticShadowDrawsSkipped==1U); CHECK(stats.persistentMaterialDescriptorHits==1U); CHECK(stats.persistentMainMaterialDescriptorHits==1U); CHECK(stats.cascadesRendered==1U);
        CHECK(stats.staticAtlasPasses==0U&&stats.dynamicAtlasPasses==1U); CHECK(stats.shadowDepthRegionsCleared==1U);
        frame.refreshStaticShadowCasters=true;
        CHECK(dve::render::record_live_environment_frame(device,lighting,shadowPlan,atlas,renderer,frame,stats,&fence,&error));
        CHECK(stats.shadowDraws==1U); CHECK(stats.staticShadowDraws==1U); CHECK(stats.persistentMaterialDescriptorHits==1U);
        CHECK(stats.staticAtlasPasses==1U&&stats.dynamicAtlasPasses==1U); CHECK(stats.shadowDepthRegionsCleared==2U);
        CHECK(renderer.shadowMaterials->invalidate_asset(triangle.contentHash,&error));
        tableStats=renderer.shadowMaterials->stats(); CHECK(tableStats.materialDescriptorCount==0U); CHECK(tableStats.imageResourceCount==2U); CHECK(tableStats.samplerResourceCount==1U);
        residencyStats=renderer.materialResidency->stats(); CHECK(residencyStats.assetReferenceCount==1U); CHECK(residencyStats.imageResourceCount==2U);
        CHECK(renderer.shadowMaterials->ensure_material(triangle,0U,&error)); CHECK(renderer.shadowMaterials->find(triangle.contentHash,0U)!=nullptr);
        residencyStats=renderer.materialResidency->stats(); CHECK(residencyStats.assetReferenceCount==2U); CHECK(residencyStats.imagesUploaded==2U); CHECK(residencyStats.samplersCreated==1U);
        CHECK(renderer.mainMaterials->invalidate_asset(triangle.contentHash,&error)); CHECK(renderer.mainMaterials->stats().materialDescriptorCount==0U);
        residencyStats=renderer.materialResidency->stats(); CHECK(residencyStats.assetReferenceCount==1U); CHECK(residencyStats.imageResourceCount==2U);
        CHECK(renderer.mainMaterials->ensure_material(triangle,0U,&error)); CHECK(renderer.mainMaterials->find(triangle.contentHash,0U)!=nullptr);
        residencyStats=renderer.materialResidency->stats(); CHECK(residencyStats.assetReferenceCount==2U); CHECK(residencyStats.imagesUploaded==2U); CHECK(residencyStats.samplersCreated==1U);

        CHECK(device.destroy_texture(colorTarget,&error)); CHECK(device.destroy_texture(depthTarget,&error));
        CHECK(dve::render::destroy_live_environment_renderer(device,renderer,&error)); mirror.reset();
        CHECK(dve::render::destroy_cascaded_shadow_atlas(device,atlas,&error)); CHECK(dve::render::destroy_environment_lighting(device,lighting,&error));
        const auto lifetime=device.statistics(); CHECK(lifetime.texturesCreated==lifetime.texturesDestroyed); CHECK(lifetime.buffersCreated==lifetime.buffersDestroyed); CHECK(lifetime.bindGroupsCreated==lifetime.bindGroupsDestroyed); CHECK(lifetime.samplersCreated==lifetime.samplersDestroyed);
        std::cout << "dve_live_environment_renderer_tests: PASS\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "dve_live_environment_renderer_tests: FAIL: " << e.what() << '\n'; return 1; }
}
