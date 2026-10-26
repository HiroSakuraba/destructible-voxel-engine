#pragma once

// The physical atmosphere's tables built on the GPU (shaders/atmosphere_*.hlsl, SPIR-V checked
// in under shaders/compiled/atmosphere): transmittance, then multiple scattering (reads
// transmittance), then the sky view (reads both), in one command list with barriers between.
// The shaders mirror src/render/atmosphere.cpp in kilometres; dve_atmosphere_gpu_tests compares
// the GPU tables with the CPU's (AtmosphereTables / AtmosphereModel) on any Vulkan device.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "dve/render/atmosphere.hpp"
#include "dve/render_environment.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

// cbuffer AtmosphereConstants (shaders/common/atmosphere.hlsli), kilometres.
struct GpuAtmosphereConstants {
    Float4 radii{};      // bottom radius, top radius, Rayleigh scale height, Mie scale height
    Float4 rayleigh{};   // Rayleigh scattering per km (rgb), Mie scattering per km
    Float4 ozone{};      // ozone absorption per km (rgb), Mie absorption per km
    Float4 shape{};      // ozone peak altitude, ozone half width, Mie anisotropy, ground albedo
    Float4 view{};       // observer altitude (km), sun zenith cosine
};
static_assert(sizeof(GpuAtmosphereConstants) == 80U);

[[nodiscard]] GpuAtmosphereConstants pack_gpu_atmosphere(const RenderEnvironment& environment) noexcept;

struct GpuAtmosphereShaders {
    std::span<const std::byte> transmittance;
    std::span<const std::byte> multipleScattering;
    std::span<const std::byte> skyView;
};

// Row-major tables of rgb (w unused), the CPU tables' sizes and layouts.
struct GpuAtmosphereTables {
    std::vector<Float4> transmittance;
    std::vector<Float4> multipleScattering;
    std::vector<Float4> skyView;
};

bool execute_gpu_atmosphere(rhi::IDevice& device, const RenderEnvironment& environment, const GpuAtmosphereShaders& shaders,
                            GpuAtmosphereTables& tables, std::string* error = nullptr);

} // namespace dve::render
