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
#include <stdexcept>
#include <vector>

namespace dve {
struct IrradianceProbe {
    std::array<Float3,9> sh{}; // real orthonormal SH, bands 0..2, m=-l..+l
    float confidence{1.0F};   // capture validity and visibility confidence
};

// LP-2A: Fit the *cosine-convolved irradiance*, rather than raw radiance,
// from an already-captured six-face HDR cubemap. Each cubemap texel is
// weighted by its exact solid angle. The SH9 cosine kernel has band gains
// pi, 2*pi/3, pi/4 for bands 0, 1 and 2 respectively.
// Capture and visibility still belong to the caller; this is a deterministic
// lighting baker, not a live GPU capture or a raytracing approximation.
[[nodiscard]] inline std::array<float,9> irradiance_sh9_basis(Float3 normal) noexcept {
    const float n2=normal.x*normal.x+normal.y*normal.y+normal.z*normal.z;
    if(!(n2>1.0e-12F) || !std::isfinite(n2))return {};
    const float inverse=1.0F/std::sqrt(n2);
    const float x=normal.x*inverse,y=normal.y*inverse,z=normal.z*inverse;
    return {0.28209479177387814F,
            0.4886025119029199F*y,0.4886025119029199F*z,0.4886025119029199F*x,
            1.0925484305920792F*x*y,1.0925484305920792F*y*z,
            0.31539156525252005F*(3.0F*z*z-1.0F),
            1.0925484305920792F*x*z,
            0.5462742152960396F*(x*x-y*y)};
}

[[nodiscard]] inline IrradianceProbe bake_irradiance_probe_sh9(
    const EnvironmentCube& radiance) {
    if(!radiance.validate())
        throw std::invalid_argument("SH9 irradiance baker requires a valid HDR cubemap");
    IrradianceProbe output{};
    constexpr float pi=3.14159265358979323846F;
    constexpr std::array<float,9> kernel{pi,
        2.0F*pi/3.0F,2.0F*pi/3.0F,2.0F*pi/3.0F,
        pi/4.0F,pi/4.0F,pi/4.0F,pi/4.0F,pi/4.0F};
    const float resolution=static_cast<float>(radiance.resolution);
    const auto area=[](double u,double v){
        return std::atan2(u*v,std::sqrt(1.0+u*u+v*v));
    };
    for(unsigned face=0;face<6U;++face){
        for(std::uint32_t y=0;y<radiance.resolution;++y){
            const double v0=2.0*static_cast<double>(y)/resolution-1.0;
            const double v1=2.0*static_cast<double>(y+1U)/resolution-1.0;
            for(std::uint32_t x=0;x<radiance.resolution;++x){
                const double u0=2.0*static_cast<double>(x)/resolution-1.0;
                const double u1=2.0*static_cast<double>(x+1U)/resolution-1.0;
                const float omega=static_cast<float>(
                    area(u1,v1)-area(u0,v1)-area(u1,v0)+area(u0,v0));
                const auto direction=cube_lookup_to_direction(
                    static_cast<CubeFace>(face),
                    {(static_cast<float>(x)+.5F)/resolution,
                     (static_cast<float>(y)+.5F)/resolution});
                const auto basis=irradiance_sh9_basis(direction);
                const auto& value=radiance.texels[
                    radiance.face_offset(static_cast<CubeFace>(face))+
                    static_cast<std::size_t>(y)*radiance.resolution+x];
                for(unsigned band=0;band<9U;++band){
                    const float scale=omega*basis[band]*kernel[band];
                    output.sh[band].x+=value.x*scale;
                    output.sh[band].y+=value.y*scale;
                    output.sh[band].z+=value.z*scale;
                }
            }
        }
    }
    return output;
}

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

// Visibility callback receives probe centre and receiver world position.
// It may raytrace a static scene or query a precomputed visibility atlas.
// Returning false discards the probe without re-normalizing its weight;
// rejected contributions must become fallback sky, not bleed through walls.
template<typename Visibility>
[[nodiscard]] inline IrradianceProbeLookup sample_irradiance_probe_grid_visible(
    const IrradianceProbeGrid& grid, Float3 position, Float3 normal,
    Visibility&& visible) {
    IrradianceProbeLookup out{};
    // The full validate() is a bake/load operation; never walk the whole
    // probe array on every shading query.
    const auto dims=grid.dimensions;
    const std::uint64_t count=static_cast<std::uint64_t>(dims[0])*dims[1]*dims[2];
    if(dims[0]==0U||dims[1]==0U||dims[2]==0U||
       dims[0]>256U||dims[1]>256U||dims[2]>256U||
       count>262144U||count!=grid.probes.size()||
       !(grid.cellSize.x>0&&grid.cellSize.y>0&&grid.cellSize.z>0)||
       !std::isfinite(grid.cellSize.x)||!std::isfinite(grid.cellSize.y)||!std::isfinite(grid.cellSize.z)||
       !std::isfinite(position.x)||!std::isfinite(position.y)||
       !std::isfinite(position.z)||!std::isfinite(normal.x)||!std::isfinite(normal.y)||
       !std::isfinite(normal.z))return out;
    const float n2=normal.x*normal.x+normal.y*normal.y+normal.z*normal.z;
    if(!(n2>1.0e-12F))return out;
    const std::array<float,9> basis=irradiance_sh9_basis(normal);
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
        const Float3 probePosition{
            grid.origin.x+grid.cellSize.x*static_cast<float>(at(0,ix!=0)),
            grid.origin.y+grid.cellSize.y*static_cast<float>(at(1,iy!=0)),
            grid.origin.z+grid.cellSize.z*static_cast<float>(at(2,iz!=0))};
        if(!visible(probePosition,position))continue;
        if(!std::isfinite(probe.confidence)||
           probe.confidence<=0.0F||probe.confidence>1.0F)continue;
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
[[nodiscard]] inline IrradianceProbeLookup sample_irradiance_probe_grid(
    const IrradianceProbeGrid& grid, Float3 position, Float3 normal) noexcept {
    return sample_irradiance_probe_grid_visible(grid,position,normal,
        [](Float3,Float3) noexcept {return true;});
}
} // namespace dve
