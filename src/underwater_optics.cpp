#include "dve/underwater_optics.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
float channel_exp(float extinction,float metres)noexcept{
 return std::exp(-std::clamp(extinction,0.F,100.F)*std::clamp(metres,0.F,10000.F));
}
float beam(float dotValue,float outer,float inner)noexcept{
 if(inner<=outer)return dotValue>=inner?1.F:0.F;
 const float t=std::clamp((dotValue-outer)/(inner-outer),0.F,1.F);
 return t*t*(3.F-2.F*t);
}
float integral(float sigma,float kd,float eyeDepth,float vz,float range)noexcept{
 // Integrate exp(-sigma*t) exp(-Kd*(depthEye-vz*t)) dt
 // along the physical ray from eye to target, with no arbitrary blue fog.
 const float rate=sigma-kd*vz;
 const float l=std::clamp(range,0.F,10000.F);
 const float numerator=std::abs(rate)<1.e-5F?l:
   -std::expm1(-rate*l)/rate;
 return std::exp(-kd*std::max(0.F,eyeDepth))*numerator;
}
bool finite(Float3 p)noexcept{return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
}
Float3 water_transmittance(const WaterVolumeSample& w,float length)noexcept{
 if(!std::isfinite(length)||length<=0)return {1,1,1};
 return {
  channel_exp(w.absorptionRGB.x+w.scatteringRGB.x,length),
  channel_exp(w.absorptionRGB.y+w.scatteringRGB.y,length),
  channel_exp(w.absorptionRGB.z+w.scatteringRGB.z,length)
 };
}
Float3 underwater_daylight(const WaterVolumeSample& w,float depth)noexcept{
 if(!std::isfinite(depth)||depth<=0)return {1,1,1};
 return {channel_exp(w.diffuseKdRGB.x,depth),channel_exp(w.diffuseKdRGB.y,depth),
         channel_exp(w.diffuseKdRGB.z,depth)};
}
Float3 evaluate_underwater_lamp(const UnderwaterLamp& lamp,
    const WaterVolumeSample& w,Float3 point)noexcept{
 const Float3 offset=subtract(point,lamp.position);
 const float r=length(offset);
 if(!(r>=.01F)||r>lamp.range||lamp.range<=0||!finite(offset)||!finite(lamp.direction))
    return {};
 const Float3 dir=normalize(lamp.direction);
 const float cone=beam(dot(multiply(offset,1.F/r),dir),lamp.outerCos,lamp.innerCos);
 if(cone<=0)return {};
 const float power=std::clamp(lamp.intensity,0.F,10000.F)*cone/(1.F+.14F*r*r);
 const auto transmission=water_transmittance(w,r);
 return {lamp.color.x*power*transmission.x,
         lamp.color.y*power*transmission.y,
         lamp.color.z*power*transmission.z};
}
Float3 shade_underwater_radiance(const WaterVolumeSample& w,
 const UnderwaterOpticsInput& in)noexcept{
 if(!std::isfinite(in.viewPathMeters)||in.viewPathMeters<=0||
    !finite(in.objectRadiance))return in.objectRadiance;
 const float l=std::clamp(in.viewPathMeters,0.F,10000.F);
 const Float3 transmission=water_transmittance(w,l);
 const auto daylight=in.objectSubmerged?
      underwater_daylight(w,in.objectDepthMeters):Float3{1,1,1};
 // Object reflectance proxy: the existing traced radiance supplies an
 // approximate albedo for the lamp, avoiding uncolored white overlays.
 const Float3 object{
   in.objectRadiance.x*(daylight.x+in.lampIrradiance.x)*transmission.x,
   in.objectRadiance.y*(daylight.y+in.lampIrradiance.y)*transmission.y,
   in.objectRadiance.z*(daylight.z+in.lampIrradiance.z)*transmission.z
 };
 // Single-scattering daylight approximation. Sunlight at each point along the
 // view path is attenuated from above before scattering toward the observer.
 // A fixed isotropic phase proxy (0.2) deliberately avoids an invented blue fog.
 const float source=0.20F;
 const float eyeDepth=std::max(0.F,in.eyeDepthMeters);
 const float vz=std::clamp(in.rayDirectionZ,-1.F,1.F);
 const Float3 haze{
   in.ambientSurfaceLight.x*source*w.scatteringRGB.x*
      integral(w.absorptionRGB.x+w.scatteringRGB.x,w.diffuseKdRGB.x,eyeDepth,vz,l),
   in.ambientSurfaceLight.y*source*w.scatteringRGB.y*
      integral(w.absorptionRGB.y+w.scatteringRGB.y,w.diffuseKdRGB.y,eyeDepth,vz,l),
   in.ambientSurfaceLight.z*source*w.scatteringRGB.z*
      integral(w.absorptionRGB.z+w.scatteringRGB.z,w.diffuseKdRGB.z,eyeDepth,vz,l)
 };
 return {std::clamp(object.x+haze.x,0.F,64.F),
         std::clamp(object.y+haze.y,0.F,64.F),
         std::clamp(object.z+haze.z,0.F,64.F)};
}
WaterRaySegment water_ray_segment(const OceanField& ocean,Float3 eye,
 Float3 dir,float metres,double time)noexcept{
 WaterRaySegment segment{};
 if(!std::isfinite(metres)||metres<=0||!finite(eye)||!finite(dir)||
    !std::isfinite(time))return segment;
 const float maxLength=std::min(metres,10000.F);
 const Float3 end=add(eye,multiply(dir,maxLength));
 const float startSide=eye.z-ocean.sample(eye.x,eye.y,time).height;
 const float endSide=end.z-ocean.sample(end.x,end.y,time).height;
 segment.cameraSubmerged=startSide<0;
 if((startSide<0)==(endSide<0)){
   if(startSide<0)segment.lengthMeters=maxLength;
   return segment;
 }
 // A fixed number of bisections keeps interpolation reproducible even when
 // the surface is wavy. Relative error < ~0.002*range with 10 iterations.
 float lo=0,hi=maxLength;
 for(int i=0;i<11;++i){
    const float mid=(lo+hi)*.5F;
    const auto p=add(eye,multiply(dir,mid));
    const float side=p.z-ocean.sample(p.x,p.y,time).height;
    if((side<0)==segment.cameraSubmerged)lo=mid;else hi=mid;
 }
 const float cut=(lo+hi)*.5F;
 segment.hitsSurface=true;
 segment.startMeters=segment.cameraSubmerged?0.F:cut;
 segment.lengthMeters=segment.cameraSubmerged?cut:(maxLength-cut);
 return segment;
}
float water_to_air_fresnel(float cosineToNormal)noexcept{
 const float ci=std::clamp(std::abs(cosineToNormal),0.F,1.F);
 constexpr float ratio=1.333F;
 const float sin2=ratio*ratio*(1-ci*ci);
 if(sin2>=1.F)return 1.F;
 const float ct=std::sqrt(std::max(0.F,1.F-sin2));
 const float rs=(ratio*ci-ct)/(ratio*ci+ct);
 const float rp=(ci-ratio*ct)/(ci+ratio*ct);
 return std::clamp((rs*rs+rp*rp)*.5F,0.F,1.F);
}
} // namespace dve
