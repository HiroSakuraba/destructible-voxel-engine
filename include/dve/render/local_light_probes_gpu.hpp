#pragma once
// LP-2B fixed, versioned shader ABI for a compact runtime local light probe payload.
// All indices/strides are shared with shaders/common/local_light_probes.hlsli.
#include "dve/irradiance_probe_grid.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace dve::render {
using ProbeFloat4=std::array<float,4>;
inline constexpr std::uint32_t kMaxLocalIrradianceProbes=1024U;
inline constexpr std::uint32_t kLocalProbeHeaderWords=4U;
inline constexpr std::uint32_t kLocalProbeStrideWords=10U;
inline constexpr std::uint32_t kLocalReflectionStrideWords=3U;
inline constexpr std::size_t kLocalProbeGpuCapacity=
    sizeof(ProbeFloat4)*(kLocalProbeHeaderWords+
        kMaxLocalIrradianceProbes*kLocalProbeStrideWords+2U*kLocalReflectionStrideWords);

struct LocalProbeGpuPayload {
    std::vector<ProbeFloat4> words;
    [[nodiscard]] bool irradiance_enabled() const noexcept {
        return !words.empty() && words[0][3]>0.0F;
    }
};

// A supplied grid must already be baked from a renderer capture. When a scene
// has no probe data, the payload still contains a disabled header so the
// global sky IBL remains exactly the same as the pre-LP2B renderer.
// refVolumes/refMips are TWO pre-selected resident cubemaps, in GPU slot order.
[[nodiscard]] inline LocalProbeGpuPayload pack_local_probe_gpu(
    const IrradianceProbeGrid* grid,
    std::span<const ReflectionProbe> refVolumes={},
    std::span<const float> refMaxMips={}) {
    LocalProbeGpuPayload p;
    p.words.resize(kLocalProbeHeaderWords);
    if(grid && grid->probes.size()<=kMaxLocalIrradianceProbes && grid->validate()) {
        const auto dims=grid->dimensions;
        p.words[0]={static_cast<float>(dims[0]),static_cast<float>(dims[1]),
                    static_cast<float>(dims[2]),1.0F};
        p.words[1]={grid->origin.x,grid->origin.y,grid->origin.z,0.0F};
        p.words[2]={grid->cellSize.x,grid->cellSize.y,grid->cellSize.z,0.0F};
        for(const auto& probe:grid->probes) {
            for(const auto& sh:probe.sh)p.words.push_back({sh.x,sh.y,sh.z,0.0F});
            p.words.push_back({probe.confidence,0.0F,0.0F,0.0F});
        }
    }
    const std::uint32_t count=static_cast<std::uint32_t>(
        std::min<std::size_t>({refVolumes.size(),refMaxMips.size(),2U}));
    std::uint32_t present=0;
    for(std::uint32_t i=0;i<count;++i) {
        const auto& r=refVolumes[i];
        if(!r.validate() || !std::isfinite(refMaxMips[i]) || refMaxMips[i]<0.0F)break;
        ++present;
    }
    p.words[3]={static_cast<float>(present),0.0F,0.0F,0.0F};
    for(std::uint32_t i=0;i<2U;++i){
        if(i<present) {
            const auto& r=refVolumes[i];
            p.words.push_back({r.position.x,r.position.y,r.position.z,
                               static_cast<float>(r.priority)});
            p.words.push_back({r.halfExtents.x,r.halfExtents.y,r.halfExtents.z,
                               r.blendDistance});
            p.words.push_back({r.intensity,refMaxMips[i],0.0F,0.0F});
        }else {
            p.words.insert(p.words.end(),3U,ProbeFloat4{});
        }
    }
    return p;
}
} // namespace dve::render
