#pragma once

#include <cstdint>
#include <string>

#include "dve/environment_lighting.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct EnvironmentLightingGpuResources {
    rhi::TextureHandle diffuseIrradiance;
    rhi::TextureViewHandle diffuseIrradianceView;
    rhi::TextureHandle specularPrefilter;
    rhi::TextureViewHandle specularPrefilterView;
    rhi::TextureHandle brdfLut;
    rhi::TextureViewHandle brdfLutView;
    rhi::SamplerHandle sampler;
    rhi::BindGroupLayoutHandle bindGroupLayout;
    rhi::BindGroupHandle bindGroup;
    std::uint32_t specularMipLevels{};
    std::uint64_t uploadedBytes{};

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool upload_environment_lighting(
    rhi::IDevice& device,
    const EnvironmentLightingAsset& asset,
    EnvironmentLightingGpuResources& resources,
    std::string* error = nullptr);

[[nodiscard]] bool destroy_environment_lighting(
    rhi::IDevice& device,
    EnvironmentLightingGpuResources& resources,
    std::string* error = nullptr);

} // namespace dve::render
