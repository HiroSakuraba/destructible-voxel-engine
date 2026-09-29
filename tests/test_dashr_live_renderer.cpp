#include "dve/environment_lighting.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/dashr_live_instance.hpp"
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

using namespace dve;
using namespace dve::render;

CookedPolygonAsset make_dashr_triangle() {
    CookedPolygonAsset asset;
    asset.objectId = 808U;

    VoxelMaterialDefinition material;
    material.name = "live DASHR PBR";
    material.baseColor = {0.55F, 0.65F, 0.85F, 1.0F};
    material.metallic = 0.15F;
    material.roughness = 0.35F;
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});

    asset.images.push_back({"height", "image/raw", 1U, 1U, {160U, 160U, 160U, 255U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"height", 0U, 0U});
    auto& binding = asset.materialBindings[0];
    binding.height.texture = 0U;
    binding.height.texcoord = 0U;
    binding.mapping.mappingMode = MaterialMappingMode::UV0;
    binding.mapping.heightScale = 0.12F;
    binding.mapping.heightReferencePlane = 0.5F;

    asset.vertices = {
        {{-0.7F,-0.7F,0.0F},{0,0,1},{1,0,0,1},{0.1F,0.9F},{1,1,1,1},{0,0}},
        {{ 0.7F,-0.7F,0.0F},{0,0,1},{1,0,0,1},{0.9F,0.9F},{1,1,1,1},{0,0}},
        {{ 0.0F, 0.7F,0.0F},{0,0,1},{1,0,0,1},{0.5F,0.1F},{1,1,1,1},{0,0}},
    };
    asset.indices = {0U,1U,2U};
    asset.submeshes.push_back({"dashr triangle", 0U, 3U, 0U});
    asset.bounds = {{-0.7F,-0.7F,0.0F},{0.7F,0.7F,0.0F}};
    asset.contentHash = polygon_asset_content_hash(asset);
    CHECK(validate_polygon_asset(asset));
    return asset;
}

DashrAtlasShaderBytecode atlas_bytecode() {
    return {{std::byte{1}}, {std::byte{2}}, {std::byte{3}}};
}

DashrLiveSurfaceConfig live_config() {
    DashrLiveSurfaceConfig config;
    config.atlasResolution = 64U;
    DashrLiveMaterialConfig material;
    material.materialIndex = 0U;
    material.traceSettings.heightScale = 0.12F;
    material.traceSettings.heightReferencePlane = 0.5F;
    material.traceSettings.envelopePadding = 0.02F;
    material.traceSettings.maximumSteps = 48U;
    material.traceSettings.refinementSteps = 3U;
    config.materials.push_back(material);
    return config;
}

LiveEnvironmentShaderBytecode live_bytecode() {
    LiveEnvironmentShaderBytecode bytecode;
    bytecode.skyboxVertex = {std::byte{1}};
    bytecode.skyboxFragment = {std::byte{2}};
    bytecode.materialVertex = {std::byte{3}};
    bytecode.materialFragment = {std::byte{4}};
    bytecode.shadowVertex = {std::byte{5}};
    bytecode.shadowFragment = {std::byte{6}};
    bytecode.dashrShell.vertex = {std::byte{7}};
    bytecode.dashrShell.fragment = {std::byte{8}};
    bytecode.dashrShell.pbrFragment = {std::byte{9}};
    bytecode.dashrShell.shadowFragment = {std::byte{10}};
    return bytecode;
}

void test_live_renderer_routes_dashr_and_preserves_shadow_order() {
    rhi::NullDevice device;
    std::string error;

    IblBakeSettings bake;
    bake.irradianceResolution = 4U;
    bake.specularResolution = 8U;
    bake.specularMipLevels = 4U;
    bake.sampleCount = 16U;
    bake.brdfResolution = 8U;
    const auto lightingAsset = make_default_environment_lighting_asset(bake);
    EnvironmentLightingGpuResources lighting;
    CHECK(upload_environment_lighting(device, lightingAsset, lighting, &error));

    camera::CameraPose camera;
    camera.position = {0, 2, 5};
    camera.target = {0,0,0};
    camera.lens.aspectRatio = 1.5F;
    camera.lens.farPlaneMeters = 100.0F;
    CascadedShadowSettings shadowSettings;
    shadowSettings.cascadeResolution = 64U;
    shadowSettings.maximumDistanceMeters = 50.0F;
    const auto shadowPlan = make_cascaded_shadow_plan(camera, {0.3F,0.8F,0.25F}, shadowSettings);
    CascadedShadowAtlasResources shadows;
    CHECK(create_cascaded_shadow_atlas(device, shadowPlan, shadows, &error));

    auto asset = make_dashr_triangle();
    MeshRhiMirror ordinaryMirror(device);
    CHECK(ordinaryMirror.upload(asset, &error));

    DashrLiveSurfaceInstance dashr(device);
    DashrLiveSurfaceUpdateStats surfaceStats;
    CHECK(dashr.initialize(asset, atlas_bytecode(), live_config(), {}, 1U, &surfaceStats, &error));
    CHECK(surfaceStats.shellBuild.emittedTriangles == 8U);

    LiveEnvironmentRendererResources renderer;
    const auto bytecode = live_bytecode();
    CHECK(create_live_environment_renderer(device, lighting, shadows, bytecode, 256U,
                                           rhi::TextureFormat::RGBA8Unorm, renderer, &error));
    CHECK(renderer.dashr_valid());

    rhi::TextureDesc color;
    color.width = 96U;
    color.height = 64U;
    color.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySource;
    color.initialState = rhi::ResourceState::RenderTarget;
    color.debugName = "DASHR integrated live color";
    rhi::TextureDesc depth = color;
    depth.format = rhi::TextureFormat::D32Float;
    depth.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::CopySource;
    depth.initialState = rhi::ResourceState::DepthWrite;
    depth.debugName = "DASHR integrated live depth";
    const auto colorTarget = device.create_texture(color, &error);
    const auto depthTarget = device.create_texture(depth, &error);
    CHECK(colorTarget && depthTarget);

    std::array<std::byte, 256> constants{};
    LivePolygonDraw draw{&ordinaryMirror, &asset, 1U};
    draw.dashr = &dashr;
    draw.castsShadow = true;
    draw.staticShadowCaster = false;

    LiveEnvironmentFrameDesc frame;
    frame.colorTarget = colorTarget;
    frame.depthTarget = depthTarget;
    frame.width = color.width;
    frame.height = color.height;
    frame.frameConstants = constants;
    frame.polygonDraws = std::span(&draw, 1U);
    frame.drawSkybox = false;
    frame.refreshStaticShadowCasters = true;
    LiveDashrViewDesc dashrView;
    dashrView.cameraWorldPosition = camera.position;
    dashrView.environmentParameters = {1.0F, 1.0F, 0.0F, 0.0F};
    frame.dashrView = dashrView;

    LiveEnvironmentFrameStats stats;
    rhi::FenceHandle frameFence;
    CHECK(record_live_environment_frame(device, lighting, shadowPlan, shadows, renderer,
                                        frame, stats, &frameFence, &error));
    CHECK(frameFence && device.fence_complete(frameFence));

    // The source polygon must be consumed by DASHR, not drawn once conventionally and once as a shell.
    CHECK(stats.materialDraws == 0U);
    CHECK(stats.shadowDraws == 0U);
    CHECK(stats.dashrSubmeshes == 1U);
    CHECK(stats.dashrMaterialDraws == 1U);
    CHECK(stats.dashrMaterialTriangles == 8U);
    CHECK(stats.dashrShadowDraws == shadowPlan.cascades.size());
    CHECK(stats.dashrShadowTriangles == 8U * shadowPlan.cascades.size());
    CHECK(stats.dashrShadowPasses == shadowPlan.cascades.size());

    // Static/dynamic atlas preparation happens before DASHR overlays. The dynamic caster is then
    // traced once per cascade into the already-cleared dynamic atlas.
    CHECK(stats.staticAtlasPasses == 1U);
    CHECK(stats.dynamicAtlasPasses == 1U);
    CHECK(stats.staticCascadesRendered == shadowPlan.cascades.size());
    CHECK(stats.dynamicCascadesRendered == shadowPlan.cascades.size());
    CHECK(stats.objectConstantRanges == 0U);
    CHECK(stats.cascadeConstantRanges == 0U);
    CHECK(dashr.last_gpu_fence() == frameFence);

    // Pose publication after a frame must synchronize with the most recent shell/shadow consumer.
    const std::vector<Float3> moved{{-0.7F,-0.7F,0.0F},{0.9F,-0.7F,0.0F},{0.0F,0.7F,0.0F}};
    DashrLiveSurfaceUpdateStats movedStats;
    CHECK(dashr.update_pose(asset, moved, 2U, &movedStats, &error));
    CHECK(dashr.pose_revision() == 2U);
    CHECK(!movedStats.skippedUnchangedPose);

    CHECK(device.destroy_texture(colorTarget, &error));
    CHECK(device.destroy_texture(depthTarget, &error));
    CHECK(destroy_live_environment_renderer(device, renderer, &error));
    CHECK(dashr.reset(&error));
    ordinaryMirror.reset();
    CHECK(destroy_cascaded_shadow_atlas(device, shadows, &error));
    CHECK(destroy_environment_lighting(device, lighting, &error));

    const auto lifetime = device.statistics();
    CHECK(lifetime.texturesCreated == lifetime.texturesDestroyed);
    CHECK(lifetime.buffersCreated == lifetime.buffersDestroyed);
    CHECK(lifetime.bindGroupsCreated == lifetime.bindGroupsDestroyed);
    CHECK(lifetime.samplersCreated == lifetime.samplersDestroyed);
}

} // namespace

int main() {
    try {
        test_live_renderer_routes_dashr_and_preserves_shadow_order();
        std::cout << "dve_dashr_live_renderer_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_dashr_live_renderer_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
