#include "dve/water_hydrodynamics.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
Float3 cross3(Float3 a,Float3 b)noexcept{return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
bool finite(Float3 p)noexcept{return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
}
bool valid_water_hull(const WaterHull& h)noexcept{
    return std::isfinite(h.massKg)&&h.massKg>=50&&h.massKg<=1.e7F&&
           std::isfinite(h.displacementCubicMetres)&&h.displacementCubicMetres>.05F&&
           h.displacementCubicMetres<=20000&&std::isfinite(h.lengthMetres)&&
           h.lengthMetres>=1&&h.lengthMetres<=200&&std::isfinite(h.beamMetres)&&
           h.beamMetres>=.5F&&h.beamMetres<=40&&std::isfinite(h.draftMetres)&&
           h.draftMetres>=.2F&&h.draftMetres<=12&&std::isfinite(h.linearDamping)&&
           h.linearDamping>=0&&h.linearDamping<=1.e7F&&std::isfinite(h.angularDamping)&&
           h.angularDamping>=0&&h.angularDamping<=1.e8F&&
           h.longitudinalProbes>=2&&h.longitudinalProbes<=12&&
           h.lateralProbes>=2&&h.lateralProbes<=8&&
           h.longitudinalProbes*h.lateralProbes<=96;
}
WaterHydroLoads evaluate_water_hull(const OceanField& ocean,double time,
        const WaterHull& hull,const WaterRigidState& body,Float3 gravity)noexcept{
    WaterHydroLoads loads{};
    if(!valid_water_hull(hull)||!finite(body.position)||!finite(body.linearVelocity)||
       !finite(body.angularVelocity))return loads;
    const float n=float(hull.longitudinalProbes*hull.lateralProbes);
    const float volume=hull.displacementCubicMetres/n;
    const float maxDepth=hull.draftMetres;
    const float rho=1025.F;
    for(unsigned j=0;j<hull.lateralProbes;++j)for(unsigned i=0;i<hull.longitudinalProbes;++i){
        const float x=(float(j)/(hull.lateralProbes-1)-.5F)*hull.beamMetres*.82F;
        const float y=(float(i)/(hull.longitudinalProbes-1)-.5F)*hull.lengthMetres*.82F;
        const Float3 worldOffset=rotate(body.orientation,{x,y,-hull.draftMetres*.48F});
        const Float3 worldPoint=add(body.position,worldOffset);
        const auto surface=ocean.sample(worldPoint.x,worldPoint.y,time);
        if(surface.depth<=.01F)continue;
        const float depth=surface.height-worldPoint.z;
        const float immersed=std::clamp(depth/maxDepth+.5F,0.0F,1.0F);
        loads.meanWaterHeight+=surface.height/n;
        loads.immersionFraction+=immersed/n;
        if(immersed<=0)continue;
        ++loads.immersedProbes;
        const Float3 velocityAt=add(body.linearVelocity,cross3(body.angularVelocity,worldOffset));
        const Float3 relative=subtract(velocityAt,surface.waterVelocity);
        Float3 force={0,0,-rho*volume*immersed*gravity.z};
        // Damping acts at each probe and supplies stabilizing pitch/roll torque.
        const float damp=hull.linearDamping/n;
        force=subtract(force,multiply(relative,damp*immersed));
        loads.forceNewton=add(loads.forceNewton,force);
        loads.torqueNewtonMetres=add(loads.torqueNewtonMetres,cross3(worldOffset,force));
        loads.impactRate+=std::max(0.0F,-relative.z)*immersed/n;
    }
    // Water resistance increases with speed and the submerged portion, but must
    // always dissipate relative motion rather than applying a constant velocity cap.
    const auto flow=ocean.sample(body.position.x,body.position.y,time).waterVelocity;
    const Float3 relative=subtract(body.linearVelocity,flow);
    loads.forceNewton=subtract(loads.forceNewton,multiply(relative,
        hull.linearDamping*loads.immersionFraction*.08F*length(relative)));
    loads.torqueNewtonMetres=subtract(loads.torqueNewtonMetres,multiply(body.angularVelocity,
        hull.angularDamping*loads.immersionFraction));
    return loads;
}
void step_water_hull(WaterRigidState& s,const WaterHull& hull,const WaterHydroLoads& loads,
        float dt,Float3 extraForce,Float3 extraTorque)noexcept{
    if(!valid_water_hull(hull)||!std::isfinite(dt)||dt<=0||dt>1.F/30)return;
    const float mass=hull.massKg;
    Float3 force=add(loads.forceNewton,extraForce);force.z-=mass*9.81F;
    s.linearVelocity=add(s.linearVelocity,multiply(force,dt/mass));
    if(length(s.linearVelocity)>120.F)s.linearVelocity=multiply(s.linearVelocity,120.F/length(s.linearVelocity));
    s.position=add(s.position,multiply(s.linearVelocity,dt));
    // Diagonal body-frame box inertia, rotated via world angular acceleration.
    const float ix=mass*(hull.lengthMetres*hull.lengthMetres+hull.draftMetres*hull.draftMetres)/12.F;
    const float iy=mass*(hull.beamMetres*hull.beamMetres+hull.draftMetres*hull.draftMetres)/12.F;
    const float iz=mass*(hull.lengthMetres*hull.lengthMetres+hull.beamMetres*hull.beamMetres)/12.F;
    const Float3 tau=add(loads.torqueNewtonMetres,extraTorque);
    const Float3 local=rotate(conjugate(s.orientation),tau);
    const Float3 angularLocal={local.x/ix,local.y/iy,local.z/iz};
    s.angularVelocity=add(s.angularVelocity,multiply(rotate(s.orientation,angularLocal),dt));
    const float rate=length(s.angularVelocity);
    if(rate>4.F)s.angularVelocity=multiply(s.angularVelocity,4.F/rate);
    if(rate>1.e-6F){
        const auto dq=quaternion_from_axis_angle(s.angularVelocity,
                    length(s.angularVelocity)*dt);
        s.orientation=normalize(multiply(dq,s.orientation));
    }
}
} 
