#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "dve/render/voxel_reference_renderer.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

dve::CookedPolygonAsset make_quad(float z, float alpha, std::uint64_t id) {
    using namespace dve;
    CookedPolygonAsset asset;
    asset.objectId = id;
    VoxelMaterialDefinition material;
    material.name = "hybrid quad";
    material.baseColor = {0.1F, 0.7F, 1.0F, alpha};
    material.roughness = 0.5F;
    if (alpha < 0.999F) {
        material.blendMode = MaterialBlendMode::Translucent;
        material.transparent = true;
    }
    asset.materials.push_back(material);
    asset.materialBindings.push_back({});
    asset.vertices = {
        {{-1.4F,-1.4F,z},{0,0,1},{1,0,0,1},{0,1},{1,1,1,1}},
        {{ 1.4F,-1.4F,z},{0,0,1},{1,0,0,1},{1,1},{1,1,1,1}},
        {{ 1.4F, 1.4F,z},{0,0,1},{1,0,0,1},{1,0},{1,1,1,1}},
        {{-1.4F, 1.4F,z},{0,0,1},{1,0,0,1},{0,0},{1,1,1,1}}};
    asset.indices = {0,1,2,0,2,3};
    asset.submeshes.push_back({"quad",0,6,0});
    asset.bounds = {{-1.4F,-1.4F,z},{1.4F,1.4F,z}};
    asset.contentHash = polygon_asset_content_hash(asset);
    return asset;
}

std::size_t find_voxel_sample(const dve::render::PolygonRenderTarget& target,
                              std::uint64_t objectId,
                              std::uint32_t materialId) {
    for (std::size_t index = 0; index < target.objectId.size(); ++index) {
        if (target.objectId[index] == objectId && target.materialIndex[index] == materialId)
            return index;
    }
    return target.objectId.size();
}

std::size_t find_overlap_sample(const dve::render::PolygonRenderTarget& voxelOnly,
                                const dve::render::PolygonRenderTarget& composite,
                                std::uint64_t voxelObjectId,
                                std::uint64_t foregroundObjectId) {
    const std::size_t count = std::min(voxelOnly.objectId.size(), composite.objectId.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (voxelOnly.objectId[index] == voxelObjectId &&
            composite.objectId[index] == foregroundObjectId)
            return index;
    }
    return count;
}
} // namespace

int main() {
    try {
        using namespace dve;
        VoxelObject voxelObject(8001U);
        // A 2x2x1 wall centered near the camera axis.
        for (int y = -1; y <= 0; ++y)
            for (int x = -1; x <= 0; ++x)
                voxelObject.set_voxel({x,y,0}, 1U);
        std::array<VoxelMaterialDefinition,2> voxelMaterials{};
        voxelMaterials[1].name = "orange voxel";
        voxelMaterials[1].baseColor = {0.9F,0.18F,0.04F,1.0F};

        render::PolygonCamera camera;
        camera.position = {0.0F,0.0F,5.0F};
        camera.target = {0.0F,0.0F,0.0F};
        camera.nearPlane = 0.1F;
        camera.farPlane = 20.0F;
        RenderEnvironment environment;
        environment.sunDirection = {-0.2F,0.7F,1.0F};
        environment.sunIntensity = 1.5F;

        const render::VoxelReferenceInstance voxelInstance{
            8001U, &voxelObject, {}, voxelMaterials, true};
        render::PolygonRenderTarget voxelOnly;
        voxelOnly.resize(64,64);
        const auto voxelStats = render::ReferenceVoxelRenderer{}.render(
            std::span<const render::VoxelReferenceInstance>(&voxelInstance,1),
            camera, environment, voxelOnly, false);
        require(voxelStats.hitRays > 100U, "authoritative voxel reference renderer missed wall");
        require(voxelStats.shadowRays == voxelStats.hitRays * environment.shadowSamples,
                "default soft-shadow sample count did not execute");
        require(voxelStats.globalIlluminationRays ==
                    voxelStats.hitRays * environment.globalIlluminationSamples,
                "default one-bounce GI sample count did not execute");
        const std::size_t voxelSample = find_voxel_sample(voxelOnly, 8001U, 1U);
        require(voxelSample < voxelOnly.objectId.size(),
                "voxel object/material IDs were not published");


        // Shadow modes have distinct bounded ray budgets, and non-traced GI modes do not
        // silently spend one-bounce rays.
        render::PolygonRenderTarget modeTarget;
        modeTarget.resize(20,20);
        RenderEnvironment hardEnvironment = environment;
        hardEnvironment.shadowMode = ShadowMode::Hard;
        hardEnvironment.globalIlluminationMode = GlobalIlluminationMode::Off;
        const auto hardStats = render::ReferenceVoxelRenderer{}.render(
            std::span<const render::VoxelReferenceInstance>(&voxelInstance,1),
            camera, hardEnvironment, modeTarget, false);
        require(hardStats.shadowRays == hardStats.hitRays,
                "hard shadows did not cast exactly one ray per visible hit");
        require(hardStats.globalIlluminationRays == 0U,
                "disabled GI still traced rays");

        RenderEnvironment contactEnvironment = environment;
        contactEnvironment.shadowMode = ShadowMode::Contact;
        contactEnvironment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
        const auto contactStats = render::ReferenceVoxelRenderer{}.render(
            std::span<const render::VoxelReferenceInstance>(&voxelInstance,1),
            camera, contactEnvironment, modeTarget, false);
        require(contactStats.shadowRays == contactStats.hitRays,
                "contact shadows did not cast one bounded near-field ray per visible hit");
        require(contactStats.globalIlluminationRays == 0U,
                "ambient-only GI unexpectedly traced scene rays");

        RenderEnvironment offEnvironment = environment;
        offEnvironment.shadowMode = ShadowMode::Off;
        offEnvironment.globalIlluminationMode = GlobalIlluminationMode::Off;
        const auto offStats = render::ReferenceVoxelRenderer{}.render(
            std::span<const render::VoxelReferenceInstance>(&voxelInstance,1),
            camera, offEnvironment, modeTarget, false);
        require(offStats.shadowRays == 0U && offStats.globalIlluminationRays == 0U,
                "disabled lighting features still traced rays");

        RenderEnvironment hybridEnvironment = environment;
        hybridEnvironment.shadowMode = ShadowMode::Hybrid;
        hybridEnvironment.shadowSamples = 3U;
        hybridEnvironment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
        const auto hybridModeStats = render::ReferenceVoxelRenderer{}.render(
            std::span<const render::VoxelReferenceInstance>(&voxelInstance,1),
            camera, hybridEnvironment, modeTarget, false);
        require(hybridModeStats.shadowRays == hybridModeStats.hitRays * 4U,
                "hybrid shadows did not cast soft samples plus one contact ray");
        require(hybridModeStats.globalIlluminationRays == 0U,
                "ambient-only GI unexpectedly traced scene rays");

        // A quad at z=1 is closer to the camera than the voxel wall around z=0.
        CookedPolygonAsset front = make_quad(1.0F,1.0F,9001U);
        render::PolygonRenderInstance frontInstance{9001U,&front,{},{},{1,1,1,1},true};
        render::PolygonRenderTarget frontHybrid;
        frontHybrid.resize(64,64);
        const std::array<render::PolygonRenderInstance,1> frontPolygons{frontInstance};
        const auto frontStats = render::render_hybrid_reference(
            std::span<const render::VoxelReferenceInstance>(&voxelInstance,1), frontPolygons,
            camera, environment, frontHybrid);
        require(frontStats.voxels.hitRays > 100U && frontStats.polygons.shadedFragments > 100U,
                "hybrid reference path did not render both geometry classes");
        const std::size_t overlapSample = find_overlap_sample(voxelOnly, frontHybrid, 8001U, 9001U);
        require(overlapSample < frontHybrid.objectId.size(),
                "front polygon did not win shared depth/ID ownership over a voxel pixel");
        const float voxelDepth = voxelOnly.depth[overlapSample];
        require(frontHybrid.depth[overlapSample] < voxelDepth,
                "front polygon depth does not use shared normalized convention");

        // A quad at z=-2 is behind the wall and must fail the voxel depth test.
        CookedPolygonAsset back = make_quad(-2.0F,1.0F,9002U);
        render::PolygonRenderInstance backInstance{9002U,&back,{},{},{1,1,1,1},true};
        render::PolygonRenderTarget backHybrid;
        backHybrid.resize(64,64);
        const std::array<render::PolygonRenderInstance,1> backPolygons{backInstance};
        const auto backStats = render::render_hybrid_reference(
            std::span<const render::VoxelReferenceInstance>(&voxelInstance,1), backPolygons,
            camera, environment, backHybrid);
        require(backStats.polygons.depthRejectedFragments > 0U,
                "back polygon did not encounter voxel depth");
        require(backHybrid.objectId[overlapSample] == 8001U,
                "voxel did not occlude polygon behind it");

        // A translucent front quad blends over the voxel while keeping opaque voxel ID ownership.
        CookedPolygonAsset glass = make_quad(1.0F,0.45F,9003U);
        render::PolygonRenderInstance glassInstance{9003U,&glass,{},{},{1,1,1,1},true};
        render::PolygonRenderTarget glassHybrid;
        glassHybrid.resize(64,64);
        const std::array<render::PolygonRenderInstance,1> glassPolygons{glassInstance};
        const auto glassStats = render::render_hybrid_reference(
            std::span<const render::VoxelReferenceInstance>(&voxelInstance,1), glassPolygons,
            camera, environment, glassHybrid);
        require(glassStats.polygons.transparentFragments > 0U,
                "front translucent polygon produced no blended fragments");
        require(glassHybrid.objectId[overlapSample] == 8001U,
                "translucent surface unexpectedly replaced opaque object ownership");
        require(std::abs(glassHybrid.hdrColor[overlapSample].x - voxelOnly.hdrColor[overlapSample].x) > 0.01F ||
                std::abs(glassHybrid.hdrColor[overlapSample].z - voxelOnly.hdrColor[overlapSample].z) > 0.01F,
                "translucent polygon did not blend over voxel color");

        std::cout << "hybrid authoritative voxel/polygon reference tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "hybrid authoritative voxel/polygon reference tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
