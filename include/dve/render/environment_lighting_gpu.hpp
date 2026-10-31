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
    // 56 uint32s: 52 packed coefficient words, one validity marker and 3 padding.
    // This buffer is always bound; a zero marker selects the legacy specular path.
    rhi::BufferHandle sheCoefficients;
    bool sheEnabled{};
    rhi::TextureViewHandle brdfLutView;
    rhi::SamplerHandle sampler;
    rhi::BindGroupLayoutHandle bindGroupLayout;
    rhi::BindGroupHandle bindGroup;
    std::uint32_t specularMipLevels{};
    std::uint64_t uploadedBytes{};
    static constexpr std::uint32_t sheValidityMarker = 0x53484531U; // SHE1

    [[nodiscard]] bool valid() const noexcept;
};

// LP-2B: GPU-resident local specular capture. Only the GGX-prefiltered
// cubemap is uploaded: each material uses the existing BRDF LUT and sampler.
// Callers own capture invalidation and retire in-flight GPU work before destroying.
struct LocalReflectionCaptureGpu {
    rhi::TextureHandle texture{};
    rhi::TextureViewHandle view{};
    std::uint32_t mipLevels{};
    [[nodiscard]] bool valid() const noexcept {
        return texture && view && mipLevels > 0U;
    }
};
[[nodiscard]] bool upload_local_reflection_capture(
    rhi::IDevice& device, const IblBakeResult& baked,
    LocalReflectionCaptureGpu& out, std::string* error = nullptr);
[[nodiscard]] bool destroy_local_reflection_capture(
    rhi::IDevice& device, LocalReflectionCaptureGpu& capture,
    std::string* error = nullptr);

[[nodiscard]] bool upload_environment_lighting(
    rhi::IDevice& device,
    const EnvironmentLightingAsset& asset,
    EnvironmentLightingGpuResources& resources,
    std::string* error = nullptr);

// Replace the probe without recreating the binding. Caller must ensure that no
// in-flight GPU frame is reading this buffer (wait for its fence / device idle).
// Passing nullptr atomically disables SHE via its zero marker, without changing layouts.
[[nodiscard]] bool update_environment_she_probe(
    rhi::IDevice& device, const SheProbeCoefficients* probe,
    EnvironmentLightingGpuResources& resources, std::string* error = nullptr);

// Rewrites the baked cubes of existing resources in place (same resolutions and mip count as the
// upload that created them); views, samplers and bind groups stay valid. Callers must make sure
// the GPU is not reading the textures.
[[nodiscard]] bool refresh_environment_lighting(
    rhi::IDevice& device,
    const EnvironmentLightingAsset& asset,
    EnvironmentLightingGpuResources& resources,
    std::string* error = nullptr);

[[nodiscard]] bool destroy_environment_lighting(
    rhi::IDevice& device,
    EnvironmentLightingGpuResources& resources,
    std::string* error = nullptr);

} // namespace dve::render
