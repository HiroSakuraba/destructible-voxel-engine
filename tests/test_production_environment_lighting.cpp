#include "dve/environment_lighting.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/rhi/null_device.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)

void test_asset_and_skybox() {
    dve::IblBakeSettings bake;
    bake.irradianceResolution=4U;
    bake.specularResolution=8U;
    bake.specularMipLevels=4U;
    bake.sampleCount=16U;
    bake.brdfResolution=8U;
    auto asset=dve::make_default_environment_lighting_asset(bake);
    std::string error;
    CHECK(asset.validate(&error));
    const auto path=std::filesystem::temp_directory_path()/"dve_production_environment_test.dveibl";
    CHECK(dve::write_dveibl(path,asset,&error));
    const auto loaded=dve::read_dveibl(path);
    CHECK(loaded);
    CHECK(loaded.asset->contentHash==asset.contentHash);
    CHECK(loaded.asset->baked.specularPrefilter.levels.size()==4U);
    std::filesystem::remove(path);

    dve::camera::CameraPose camera;
    camera.position={0.0F,1.0F,5.0F};
    camera.target={0.0F,1.0F,0.0F};
    camera.lens.aspectRatio=16.0F/9.0F;
    dve::EnvironmentSkyboxSettings settings;
    settings.width=32U;settings.height=18U;settings.intensity=1.2F;settings.exposure=0.5F;
    const auto image=dve::render_environment_skybox(asset.sourceRadiance,camera,settings);
    CHECK(image.size()==576U);
    for(const auto pixel:image) CHECK(std::isfinite(pixel.x)&&std::isfinite(pixel.y)&&std::isfinite(pixel.z)&&pixel.w==1.0F);
    const auto rotatedDirection=dve::rotate_environment_direction({1.0F,0.0F,0.0F},1.57079632679F);
    CHECK(std::abs(rotatedDirection.x)<1.0e-4F);
    CHECK(rotatedDirection.z<-0.999F);
}

void test_gpu_upload_and_shadow_atlas() {
    dve::IblBakeSettings bake;
    bake.irradianceResolution=4U;bake.specularResolution=8U;bake.specularMipLevels=4U;bake.sampleCount=16U;bake.brdfResolution=8U;
    const auto asset=dve::make_default_environment_lighting_asset(bake);
    dve::rhi::NullDevice device;
    dve::render::EnvironmentLightingGpuResources lighting;
    std::string error;
    CHECK(dve::render::upload_environment_lighting(device,asset,lighting,&error));
    CHECK(lighting.valid());
    CHECK(lighting.uploadedBytes>0U);
    std::vector<std::byte> face(static_cast<std::size_t>(bake.irradianceResolution)*bake.irradianceResolution*8U);
    CHECK(device.read_texture(lighting.diffuseIrradiance,0U,0U,face,static_cast<std::size_t>(bake.irradianceResolution)*8U,&error));
    bool nonzero=false;for(const auto value:face)nonzero|=std::to_integer<unsigned char>(value)!=0U;CHECK(nonzero);

    dve::camera::CameraPose camera;
    camera.position={0.0F,3.0F,8.0F};camera.target={0.0F,1.0F,0.0F};camera.lens.nearPlaneMeters=0.1F;camera.lens.farPlaneMeters=500.0F;camera.lens.aspectRatio=16.0F/9.0F;
    dve::render::CascadedShadowSettings settings;settings.cascadeResolution=256U;settings.maximumDistanceMeters=100.0F;
    const auto cascades=dve::render::make_cascaded_shadow_plan(camera,{0.4F,0.8F,0.2F},settings);
    dve::render::CascadedShadowAtlasResources atlas;
    CHECK(dve::render::create_cascaded_shadow_atlas(device,cascades,atlas,&error));
    CHECK(atlas.valid());
    const auto frame=dve::render::make_cascaded_shadow_atlas_frame_plan(cascades,atlas,{1U,3U});
    CHECK(frame.validate(cascades,&error));
    CHECK(!frame.clearAtlas);
    CHECK(frame.regions.size()==4U);
    CHECK(!frame.regions[0].dirty&&frame.regions[1].dirty&&!frame.regions[2].dirty&&frame.regions[3].dirty);
    CHECK(!frame.regions[0].clearBeforeDraw&&frame.regions[1].clearBeforeDraw&&
          !frame.regions[2].clearBeforeDraw&&frame.regions[3].clearBeforeDraw);
    const auto dynamicFrame=dve::render::make_cascaded_shadow_atlas_frame_plan(
        cascades,atlas,{0U},dve::render::CascadedShadowLayer::Dynamic);
    CHECK(dynamicFrame.renderPass.depth->texture==atlas.dynamicDepthAtlas);
    CHECK(dynamicFrame.regions[0].clearBeforeDraw);
    const auto coordinate=dve::render::cascaded_shadow_atlas_coordinate(cascades,0U,cascades.cascades[0].snappedCenter);
    CHECK(coordinate.x>=0.0F&&coordinate.x<=1.0F&&coordinate.y>=0.0F&&coordinate.y<=1.0F);

    CHECK(dve::render::destroy_cascaded_shadow_atlas(device,atlas,&error));
    CHECK(dve::render::destroy_environment_lighting(device,lighting,&error));
    const auto statistics=device.statistics();
    CHECK(statistics.texturesCreated==statistics.texturesDestroyed);
    CHECK(statistics.samplersCreated==statistics.samplersDestroyed);
    CHECK(statistics.bindGroupsCreated==statistics.bindGroupsDestroyed);
}
}

int main(){try{test_asset_and_skybox();test_gpu_upload_and_shadow_atlas();std::cout<<"dve_production_environment_lighting_tests: PASS\n";return 0;}catch(const std::exception& exception){std::cerr<<"dve_production_environment_lighting_tests: FAIL: "<<exception.what()<<'\n';return 1;}}
