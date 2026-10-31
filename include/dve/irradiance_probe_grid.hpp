#pragma once
// LP-1 diffuse irradiance light-probe grid: CPU reference and future GPU bake ABI.
// SH9 coefficients represent incoming *irradiance*, after cosine convolution.
// This is NOT a runtime GPU capture or a replacement for dynamic traced GI.
#include "dve/environment_lighting.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dve {
struct IrradianceProbe {
    std::array<Float3,9> sh{}; // real orthonormal SH, bands 0..2, m=-l..+l
    float confidence{1.0F};   // capture validity and visibility confidence
};

struct IrradianceProbeGrid {
    Float3 origin{};
    Float3 cellSize{1,1,1};
    std::array<std::uint32_t,3> dimensions{1,1,1};
    std::vector<IrradianceProbe> probes; // x-fastest, then y, then z

    [[nodiscard]] bool validate() const noexcept {
        const auto finite=[](Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
        if(!finite(origin)||!finite(cellSize)||cellSize.x<=0||cellSize.y<=0||cellSize.z<=0)return false;
        std::uint64_t count=1;
        for(const auto n:dimensions){
            if(n<1U||n>256U)return false;
            count*=n;
        }
        if(count!=probes.size()||count>262144U)return false;
        for(const auto& p:probes){
            if(!std::isfinite(p.confidence)||p.confidence<0||p.confidence>1)return false;
            for(const auto& c:p.sh)if(!finite(c))return false;
        }
        return true;
    }
};

struct IrradianceProbeLookup {
    Float3 irradiance{};
    float coverage{};  // 0..1; residual weight belongs to the fallback sky
};

[[nodiscard]] inline IrradianceProbeLookup sample_irradiance_probe_grid(
    const IrradianceProbeGrid& grid, Float3 position, Float3 normal) noexcept {
    IrradianceProbeLookup out{};
    if(!grid.validate()||!std::isfinite(position.x)||!std::isfinite(position.y)||
       !std::isfinite(position.z)||!std::isfinite(normal.x)||!std::isfinite(normal.y)||
       !std::isfinite(normal.z))return out;
    const float n2=normal.x*normal.x+normal.y*normal.y+normal.z*normal.z;
    if(!(n2>1.0e-12F))return out;
    const float inv=1.0F/std::sqrt(n2),x=normal.x*inv,y=normal.y*inv,z=normal.z*inv;
    const std::array<float,9> basis{
        0.28209479177387814F,
        0.4886025119029199F*y,0.4886025119029199F*z,0.4886025119029199F*x,
        1.0925484305920792F*x*y,1.0925484305920792F*y*z,
        0.31539156525252005F*(3.0F*z*z-1.0F),
        1.0925484305920792F*x*z,
        0.5462742152960396F*(x*x-y*y)
    };
    const std::array<float,3> world{position.x,position.y,position.z};
    const std::array<float,3> origin{grid.origin.x,grid.origin.y,grid.origin.z};
    const std::array<float,3> cells{grid.cellSize.x,grid.cellSize.y,grid.cellSize.z};
    std::array<std::uint32_t,3> lo{},hi{};
    std::array<float,3> fraction{};
    for(int i=0;i<3;++i){
        const float u=(world[i]-origin[i])/cells[i];
        if(!std::isfinite(u)||u<0||u>static_cast<float>(grid.dimensions[i]-1U))return out;
        lo[i]=static_cast<std::uint32_t>(std::floor(u));
        hi[i]=std::min(lo[i]+1U,grid.dimensions[i]-1U);
        fraction[i]=u-static_cast<float>(lo[i]);
    }
    const std::size_t strideY=grid.dimensions[0],strideZ=strideY*grid.dimensions[1];
    for(int iz=0;iz<2;++iz)for(int iy=0;iy<2;++iy)for(int ix=0;ix<2;++ix){
        const float geometric=(ix?fraction[0]:1-fraction[0])*
                              (iy?fraction[1]:1-fraction[1])*
                              (iz?fraction[2]:1-fraction[2]);
        if(geometric<=0)continue;
        const auto at=[&](int axis,bool upper){return upper?hi[axis]:lo[axis];};
        const std::size_t index=at(0,ix!=0)+strideY*at(1,iy!=0)+strideZ*at(2,iz!=0);
        const auto& probe=grid.probes[index];
        const float weight=geometric*probe.confidence;
        if(weight<=0)continue;
        Float3 irradiance{};
        for(std::size_t i=0;i<basis.size();++i){
            irradiance.x+=basis[i]*probe.sh[i].x;
            irradiance.y+=basis[i]*probe.sh[i].y;
            irradiance.z+=basis[i]*probe.sh[i].z;
        }
        // Negative lobes are a finite-SH fitting artifact, not negative light.
        out.irradiance.x+=weight*std::max(irradiance.x,0.0F);
        out.irradiance.y+=weight*std::max(irradiance.y,0.0F);
        out.irradiance.z+=weight*std::max(irradiance.z,0.0F);
        out.coverage+=weight;
    }
    out.coverage=std::clamp(out.coverage,0.0F,1.0F);
    return out; // energy-weighted: add (1 - coverage) * sky irradiance
}
} // namespace dve
