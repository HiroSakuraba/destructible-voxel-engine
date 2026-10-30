#include "dve_test_skip.hpp"
#include "dve/environment_lighting.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/rhi/vulkan_device.hpp"

#include <array>
#include <span>
#include <cstdint>
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
            return dve::testing::skip(std::string("Vulkan environment lighting test: ")+std::string(device.device_loss_reason()));
        }
        dve::IblBakeSettings settings;settings.irradianceResolution=4U;settings.specularResolution=8U;settings.specularMipLevels=4U;settings.sampleCount=16U;settings.brdfResolution=8U;
        const auto asset=dve::make_default_environment_lighting_asset(settings);
        dve::render::EnvironmentLightingGpuResources lighting;
        std::string error;
        require(dve::render::upload_environment_lighting(device,asset,lighting,&error),error.c_str());
        std::vector<std::byte> readback(static_cast<std::size_t>(settings.irradianceResolution)*settings.irradianceResolution*8U);
        require(device.read_texture(lighting.diffuseIrradiance,0U,0U,readback,static_cast<std::size_t>(settings.irradianceResolution)*8U,&error),error.c_str());
        bool nonzero=false;for(const auto value:readback)nonzero|=std::to_integer<unsigned char>(value)!=0U;require(nonzero,"Vulkan irradiance readback is zero");
        require(!lighting.sheEnabled,"SHE must be disabled for a legacy environment");
        std::array<std::uint32_t,56> sheWords{};
        require(device.read_buffer(lighting.sheCoefficients,0U,
                std::as_writable_bytes(std::span(sheWords)),&error),error.c_str());
        require(sheWords[52]==0U,"legacy environment must not enable SHE in GPU shader");
        dve::SheProbeCoefficients fitted;
        fitted.rgb[0][0]=1.0F;fitted.rgb[1][3]=-0.3F;
        require(dve::render::update_environment_she_probe(device,&fitted,lighting,&error),error.c_str());
        require(lighting.sheEnabled,"valid SHE probe not marked enabled");
        require(device.read_buffer(lighting.sheCoefficients,0U,
                std::as_writable_bytes(std::span(sheWords)),&error),error.c_str());
        require(sheWords[52]==dve::render::EnvironmentLightingGpuResources::sheValidityMarker,
                "Vulkan shader-visible SHE marker upload mismatch");
        require(dve::render::update_environment_she_probe(device,nullptr,lighting,&error),error.c_str());
        require(!lighting.sheEnabled,"SHE disable did not clear enabled state");

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
        require(stats.buffersCreated==stats.buffersDestroyed,"Vulkan SHE buffer lifetime imbalance");
        require(stats.bindGroupsCreated==stats.bindGroupsDestroyed,"Vulkan environment bind-group lifetime imbalance");
        std::cout<<"dve_vulkan_environment_lighting_tests: PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<"dve_vulkan_environment_lighting_tests: FAIL: "<<e.what()<<'\n';return 1;}
}
