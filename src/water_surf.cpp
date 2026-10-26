#include "dve/water_surf.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
Float3 cross3(Float3 a,Float3 b)noexcept{return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
bool finite(Float3 p)noexcept{return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
}
SurfLoads evaluate_surfboard(const WaterHull& hull,const WaterRigidState& body,
 const OceanSample& water,const SurfInput& ctrl,float immersion)noexcept{
 SurfLoads result{};
 if(!valid_water_hull(hull)||!finite(body.position)||!finite(body.linearVelocity)||
    !finite(body.angularVelocity)||!finite(water.normal)||!finite(water.waterVelocity)||
    !std::isfinite(ctrl.lean)||!std::isfinite(ctrl.paddle)||
    !std::isfinite(immersion))return result;
 const float wet=std::clamp(immersion,0.F,1.F);
 if(wet<=0)return result;
 const Float3 f=rotate(body.orientation,{0,1,0});
 const Float3 right=rotate(body.orientation,{1,0,0});
 const Float3 up=rotate(body.orientation,{0,0,1});
 const Float3 delta=subtract(body.linearVelocity,water.waterVelocity);
 const float ahead=dot(delta,f),side=dot(delta,right);
 const float lean=std::clamp(ctrl.lean,-1.F,1.F);
 const float paddle=std::clamp(ctrl.paddle,-1.F,1.F);
 // The normal encodes the local wave face. A board can trade gravitational
 // potential for horizontal kinetic energy, rather than getting arbitrary thrust.
 const float nz=std::max(.2F,water.normal.z);
 const Float3 downhill={water.normal.x/nz,water.normal.y/nz,0};
 const float mass=hull.massKg;
 const Float3 face=multiply(downhill,mass*9.81F*.62F*wet);
 result.force=add(result.force,face);
 result.faceDrive=dot(face,f);
 // Small rider paddling effort is the only unearned propulsion.
 result.force=add(result.force,multiply(f,paddle*mass*1.45F*wet));
 // Underwater fin + rail resists lateral slip, allowing carving arcs.
 const float resistance=std::clamp((1.4F+std::abs(ahead)*.32F)*mass*wet,0.F,4500.F);
 const Float3 rail=multiply(right,-side*resistance);
 result.force=add(result.force,rail);
 result.railForce=dot(rail,right);
 // Carving is a fin/rail force on the water, not an instantaneous yaw rotation.
 const float turnForce=lean*mass*std::min(std::abs(ahead),12.F)*1.8F*wet;
 const Float3 carve=multiply(right,turnForce);
 result.force=add(result.force,carve);
 const Float3 stern=rotate(body.orientation,{0,-hull.lengthMetres*.37F,-.1F});
 result.torque=add(result.torque,cross3(stern,carve));
 // Surface-alignment torque and angular damping smooth pitch and roll.
 const Float3 align=cross3(up,water.normal);
 result.torque=add(result.torque,multiply(align,mass*9.81F*.26F*wet));
 result.torque=subtract(result.torque,multiply(body.angularVelocity,mass*2.2F*wet));
 result.breakIntensity=water.breaking;
 const float roll=std::abs(dot(right,{0,0,1}));
 result.stability=std::clamp(1.F-roll-water.breaking*.2F,0.F,1.F);
 result.wipeout=(roll>.85F&&std::abs(ahead)>2.F)||
   (water.breaking>.87F&&std::abs(ahead)<1.2F)||
   (water.depth<.18F);
 return result;
}
}
