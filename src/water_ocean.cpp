#include "dve/water_ocean.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
constexpr float g=9.81F;
constexpr float tau=6.283185307179586F;
struct Point {
    Float3 p{};
    Float3 dx{1,0,0},dy{0,1,0},dt{};
    float foam{};
};
Point evaluate(std::span<const OceanWave> waves,float x,float y,double seconds){
    Point out{};out.p={x,y,0};
    for(const OceanWave& w:waves){
        const float k=tau/w.wavelength;
        const float omega=std::sqrt(g*k);
        const float theta=std::remainder(double(k)*(w.direction.x*x+w.direction.y*y)-
                 double(omega)*seconds+double(w.phase),double(tau));
        const float s=std::sin(theta),c=std::cos(theta);
        const float qa=w.steepness*w.amplitude;
        out.p.x+=qa*w.direction.x*c;out.p.y+=qa*w.direction.y*c;out.p.z+=w.amplitude*s;
        out.dx.x-=qa*k*w.direction.x*w.direction.x*s;
        out.dx.y-=qa*k*w.direction.y*w.direction.x*s;
        out.dx.z+=w.amplitude*k*w.direction.x*c;
        out.dy.x-=qa*k*w.direction.x*w.direction.y*s;
        out.dy.y-=qa*k*w.direction.y*w.direction.y*s;
        out.dy.z+=w.amplitude*k*w.direction.y*c;
        out.dt.x+=qa*omega*w.direction.x*s;
        out.dt.y+=qa*omega*w.direction.y*s;
        out.dt.z-=w.amplitude*omega*c;
        out.foam+=std::max(0.0F,w.steepness*k*w.amplitude*(-s));
    }
    return out;
}
}
bool valid_ocean_wave(const OceanWave& w)noexcept {
    const float l2=w.direction.x*w.direction.x+w.direction.y*w.direction.y;
    return std::isfinite(l2)&&std::abs(l2-1.0F)<0.03F&&
           std::isfinite(w.amplitude)&&w.amplitude>=0&&w.amplitude<=18&&
           std::isfinite(w.wavelength)&&w.wavelength>=1&&w.wavelength<=1000&&
           std::isfinite(w.phase)&&std::abs(w.phase)<=1000000&&
           std::isfinite(w.steepness)&&w.steepness>=0&&w.steepness<=1.0F;
}
bool OceanField::set_waves(std::span<const OceanWave> waves,Float3 current)noexcept{
    if(waves.size()>waves_.size()||!std::isfinite(current.x)||!std::isfinite(current.y)||
       !std::isfinite(current.z)||std::abs(current.x)>25||std::abs(current.y)>25||
       std::abs(current.z)>5)return false;
    float limit=0;
    for(const auto& w:waves){
        if(!valid_ocean_wave(w))return false;
        // Keep horizontal Gerstner displacement invertible under combined steepening.
        limit+=w.steepness*(tau/w.wavelength)*w.amplitude;
    }
    if(limit>0.83F)return false;
    count_=std::uint32_t(waves.size());maxHeight_=0;current_=current;
    for(std::size_t i=0;i<waves.size();++i){waves_[i]=waves[i];maxHeight_+=waves[i].amplitude;}
    return true;
}
bool OceanField::set_preset(OceanPreset preset,Float3 current)noexcept{
    if(preset>OceanPreset::Storm)return false;
    // Seven broad components for a breeze, ten for storms; small aggregate steepness
    // avoids Gerstner foldovers, with high-frequency appearance left to the material.
    constexpr std::array<float,10> periods{64,41,27,19,94,13,8,112,6,33};
    constexpr std::array<float,10> angles{.15F,-.18F,.33F,-.41F,.7F,-.75F,.46F,.82F,-.57F,.12F};
    constexpr std::array<float,10> phases{0,.5F,2.2F,1.1F,3.2F,4.1F,5.2F,.3F,2.9F,5.4F};
    constexpr std::array<float,10> heights{1.0F,.45F,.25F,.14F,.30F,.08F,.05F,.38F,.03F,.10F};
    std::array<OceanWave,10> w{};
    const unsigned count=preset==OceanPreset::Calm?0U:preset==OceanPreset::Breeze?7U:10U;
    const float factor=preset==OceanPreset::Storm?2.3F:0.55F;
    for(unsigned i=0;i<count;++i){
        const float a=angles[i];w[i].direction={std::sin(a),std::cos(a)};
        w[i].wavelength=periods[i];w[i].amplitude=heights[i]*factor;
        w[i].phase=phases[i];w[i].steepness=0.38F;
    }
    if(!set_waves(std::span<const OceanWave>(w.data(),count),current))return false;
    preset_=preset;
    return true;
}
OceanSample OceanField::sample(float x,float y,double seconds)const noexcept{
    OceanSample result{};
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(seconds))return result;
    if(count_==0){result.waterVelocity=current_;return result;}
    float u=x,v=y;
    const auto waveSpan=waves();
    // Invert horizontal displacement so the query is at the *world-space* x,y
    // rather than at the undisplaced parametric coordinate.
    for(unsigned i=0;i<4;++i){
        const Point p=evaluate(waveSpan,u,v,seconds);
        const float ex=p.p.x-x,ey=p.p.y-y;
        const float det=p.dx.x*p.dy.y-p.dy.x*p.dx.y;
        if(std::abs(det)<0.1F)break;
        const float du=(p.dy.y*ex-p.dy.x*ey)/det;
        const float dv=(-p.dx.y*ex+p.dx.x*ey)/det;
        u-=std::clamp(du,-12.0F,12.0F);
        v-=std::clamp(dv,-12.0F,12.0F);
        if(std::abs(du)+std::abs(dv)<.0005F)break;
    }
    const Point p=evaluate(waveSpan,u,v,seconds);
    const Float3 n{p.dx.y*p.dy.z-p.dx.z*p.dy.y,
                   p.dx.z*p.dy.x-p.dx.x*p.dy.z,
                   p.dx.x*p.dy.y-p.dx.y*p.dy.x};
    result.height=p.p.z;
    result.normal=length(n)>1.e-6F?normalize(n):Float3{0,0,1};
    if(result.normal.z<0)result.normal=multiply(result.normal,-1);
    result.waterVelocity=add(p.dt,current_);
    result.foam=std::clamp(p.foam*1.3F,0.0F,1.0F);
    return result;
}
}
