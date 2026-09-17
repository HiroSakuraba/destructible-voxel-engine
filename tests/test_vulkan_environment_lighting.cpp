#include "dve/environment_lighting.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/rhi/vulkan_device.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}

int main(){
    try{
        dve::rhi::VulkanDevice device;
        if(device.status()!=dve::rhi::DeviceStatus::Ready){
            if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(std::string("required Vulkan unavailable: ")+std::string(device.device_loss_reason()));
            std::cout<<"Vulkan environment lighting test skipped: "<<device.device_loss_reason()<<'\n';return 0;
        }
        dve::IblBakeSettings settings;settings.irradianceResolution=4U;settings.specularResolution=8U;settings.specularMipLevels=4U;settings.sampleCount=16U;settings.brdfResolution=8U;
        const auto asset=dve::make_default_environment_lighting_asset(settings);
        dve::render::EnvironmentLightingGpuResources lighting;
        std::string error;
        require(dve::render::upload_environment_lighting(device,asset,lighting,&error),error.c_str());
        std::vector<std::byte> readback(static_cast<std::size_t>(settings.irradianceResolution)*settings.irradianceResolution*8U);
        require(device.read_texture(lighting.diffuseIrradiance,0U,0U,readback,static_cast<std::size_t>(settings.irradianceResolution)*8U,&error),error.c_str());
        bool nonzero=false;for(const auto value:readback)nonzero|=std::to_integer<unsigned char>(value)!=0U;require(nonzero,"Vulkan irradiance readback is zero");

        dve::camera::CameraPose camera;camera.position={0,3,8};camera.target={0,1,0};camera.lens.aspectRatio=16.0F/9.0F;camera.lens.farPlaneMeters=500.0F;
        dve::render::CascadedShadowSettings shadow;shadow.cascadeResolution=256U;shadow.maximumDistanceMeters=100.0F;
        const auto plan=dve::render::make_cascaded_shadow_plan(camera,{0.4F,0.8F,0.2F},shadow);
        dve::render::CascadedShadowAtlasResources atlas;
        require(dve::render::create_cascaded_shadow_atlas(device,plan,atlas,&error),error.c_str());
        require(atlas.valid(),"Vulkan shadow atlas resources are incomplete");
        require(dve::render::destroy_cascaded_shadow_atlas(device,atlas,&error),error.c_str());
        require(dve::render::destroy_environment_lighting(device,lighting,&error),error.c_str());
        const auto stats=device.statistics();
        require(stats.texturesCreated==stats.texturesDestroyed,"Vulkan environment texture lifetime imbalance");
        require(stats.samplersCreated==stats.samplersDestroyed,"Vulkan environment sampler lifetime imbalance");
        require(stats.bindGroupsCreated==stats.bindGroupsDestroyed,"Vulkan environment bind-group lifetime imbalance");
        std::cout<<"dve_vulkan_environment_lighting_tests: PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<"dve_vulkan_environment_lighting_tests: FAIL: "<<e.what()<<'\n';return 1;}
}
