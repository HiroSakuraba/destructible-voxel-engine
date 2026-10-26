#include "dve/water_volume.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
bool valid_water_volume_settings(const WaterVolumeSettings& s)noexcept{
 const auto validOptical=[](Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&
   std::isfinite(v.z)&&v.x>=0&&v.y>=0&&v.z>=0&&
   v.x<=10&&v.y<=10&&v.z<=10;};
 return (!s.customOptics||(validOptical(s.absorptionRGB)&&
         validOptical(s.scatteringRGB)&&validOptical(s.diffuseKdRGB)))&&
   std::isfinite(s.floorZ)&&s.floorZ>=-5000&&s.floorZ<=-3&&
        std::isfinite(s.density)&&s.density>=995&&s.density<=1040&&
        s.clarity<=WaterClarity::Turbid;
}
WaterVolumeSample sample_water_volume(const OceanField& ocean,
  const WaterVolumeSettings& cfg,Float3 p,double seconds)noexcept{
 WaterVolumeSample out{};
 if(!valid_water_volume_settings(cfg)||!std::isfinite(p.x)||!std::isfinite(p.y)||
    !std::isfinite(p.z)||!std::isfinite(seconds))return out;
 const auto sea=ocean.sample(p.x,p.y,seconds);
 out.surfaceHeight=sea.height;out.seafloorHeight=cfg.floorZ;
 out.depthBelowSurface=std::max(0.F,sea.height-p.z);
 out.pressurePascal=101325.F+cfg.density*9.81F*out.depthBelowSurface;
 out.density=cfg.density;out.submerged=p.z<sea.height;
 // Deep water retains mean current but cannot inherit the surface's full
 // vertical/rotational orbital velocity. The depth scale is characteristic,
 // not an exact multi-wavelength dispersion calculation.
 const float attenuation=std::exp(-out.depthBelowSurface/5.F);
 out.flowVelocity=add(ocean.current(),multiply(subtract(sea.waterVelocity,ocean.current()),attenuation));
 switch(cfg.clarity){
 case WaterClarity::ClearOcean:
   out.absorptionRGB={.34F,.075F,.018F};out.scatteringRGB={.018F,.015F,.012F};
   out.diffuseKdRGB={.38F,.10F,.045F};out.turbidity=.05F;out.particleDensity=.01F;break;
 case WaterClarity::Coastal:
   out.absorptionRGB={.45F,.14F,.10F};out.scatteringRGB={.30F,.25F,.23F};
   out.diffuseKdRGB={.60F,.38F,.38F};out.turbidity=.4F;out.particleDensity=.12F;break;
 case WaterClarity::Turbid:
   out.absorptionRGB={.60F,.30F,.28F};out.scatteringRGB={1.3F,1.2F,1.1F};
   out.diffuseKdRGB={1.5F,1.4F,1.3F};out.turbidity=.9F;out.particleDensity=.55F;break;
 }
 if(cfg.customOptics){
   out.absorptionRGB=cfg.absorptionRGB;out.scatteringRGB=cfg.scatteringRGB;
   out.diffuseKdRGB=cfg.diffuseKdRGB;
 }
 return out;
}
} // namespace dve
