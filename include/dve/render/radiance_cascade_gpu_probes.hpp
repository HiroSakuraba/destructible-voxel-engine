#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

// Compact primary-hit bridge for the first GPU radiance-cascade pass. The shader uses the
// same 32-byte StructuredBuffer stride; material data is consumed by later passes.
struct RadianceCascadeSurfaceSample {
    Float3 position{};
    float viewDistance{};
    Float3 normal{};
    std::uint32_t valid{};
};
static_assert(sizeof(RadianceCascadeSurfaceSample) == 32);

struct RadianceCascadeProbeConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t spacing{};
    std::uint32_t probesX{};
    std::uint32_t probesY{};
    std::uint32_t padding[3]{};
};
static_assert(sizeof(RadianceCascadeProbeConstants) == 32);

// Throws std::invalid_argument for malformed dimensions/levels. Invalid texels are all zero.
[[nodiscard]] std::vector<RadianceCascadeSurfaceSample> pack_radiance_cascade_primary(
    const spwi::VoxelGBuffer& gbuffer);
[[nodiscard]] std::vector<RadianceCascadeSurfaceSample> reference_radiance_cascade_probes(
    const spwi::VoxelGBuffer& gbuffer, const spwi::CascadeLevelInfo& level);

// Executes only BuildRadianceCascadeProbes offscreen. Bytecode is the compiled
// rc_build_probes.hlsl SPIR-V (or native backend equivalent). Returns false with an error on
// allocation, dispatch, or readback failure; no resource escapes this call.
bool execute_radiance_cascade_probe_pass(rhi::IDevice& device,
    const spwi::VoxelGBuffer& gbuffer, const spwi::CascadeLevelInfo& level,
    std::span<const std::byte> bytecode, std::vector<RadianceCascadeSurfaceSample>& probes,
    std::string* error = nullptr);

} // namespace dve::render
