#include "dve/water_vessel_dynamics.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
constexpr float waterDensity=1025.F;
constexpr float airDensity=1.225F;
Float3 cross3(Float3 a,Float3 b)noexcept{
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
bool finite(Float3 p)noexcept{return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
Float3 horizontal(Float3 x)noexcept{return {x.x,x.y,0};}
}
const char* watercraft_name(WatercraftType kind)noexcept{
    switch(kind){
        case WatercraftType::Dinghy:return "DINGHY";
        case WatercraftType::Sailboat:return "SAILBOAT";
        case WatercraftType::PirateShip:return "PIRATE SHIP";
        case WatercraftType::Speedboat:return "SPEEDBOAT";
        case WatercraftType::WaveRunner:return "WAVE RUNNER";
        case WatercraftType::MiniSub:return "MINI-SUB";
    }
    return "VESSEL";
}
bool valid_vessel_dynamics(const VesselDynamics& p)noexcept{
    return (p.type<=WatercraftType::WaveRunner||p.type==WatercraftType::MiniSub)&&
        std::isfinite(p.sailArea)&&p.sailArea>=0&&p.sailArea<=10000&&
        std::isfinite(p.keelArea)&&p.keelArea>=0&&p.keelArea<=200&&
        std::isfinite(p.mastHeight)&&p.mastHeight>=0&&p.mastHeight<=90&&
        std::isfinite(p.thrustNewtons)&&p.thrustNewtons>=0&&p.thrustNewtons<=1.e7F&&
        std::isfinite(p.rudderNewtons)&&p.rudderNewtons>=0&&p.rudderNewtons<=1.e7F&&
        std::isfinite(p.planingFactor)&&p.planingFactor>=0&&p.planingFactor<=2;
}
VesselDriveLoads evaluate_vessel_dynamics(
   const VesselDynamics& p,const WaterHull& hull,const WaterRigidState& body,
   const OceanSample& ocean,Float3 windVelocity,const VesselInput& control,
   float submerged)noexcept {
    VesselDriveLoads result{};
    if(!valid_vessel_dynamics(p)||!valid_water_hull(hull)||!finite(windVelocity)||
       !finite(body.linearVelocity)||!finite(ocean.waterVelocity)||
       !std::isfinite(control.throttle)||!std::isfinite(control.helm)||
       !std::isfinite(control.sailTrim)||!std::isfinite(submerged))return result;
    submerged=std::clamp(submerged,0.F,1.F);
    const float helm=std::clamp(control.helm,-1.F,1.F);
    const float throttle=std::clamp(control.throttle,-1.F,1.F);
    const Float3 forward=rotate(body.orientation,{0,1,0});
    const Float3 right=rotate(body.orientation,{1,0,0});
    const Float3 relativeWater=horizontal(subtract(body.linearVelocity,ocean.waterVelocity));
    const float along=dot(relativeWater,forward),side=dot(relativeWater,right);
    result.forwardWaterSpeed=along;result.lateralWaterSpeed=side;
    const Float3 airFlow=horizontal(subtract(windVelocity,body.linearVelocity));
    const float airSpeed=length(airFlow);
    result.apparentWind=airSpeed;
    if(p.sailArea>0&&airSpeed>.1F) {
        const float trim=std::clamp(control.sailTrim,-1.4F,1.4F);
        const Float3 chord=normalize(add(multiply(forward,std::cos(trim)),
                                          multiply(right,std::sin(trim))));
        const Float3 flowDir=multiply(airFlow,1.F/airSpeed);
        const Float3 sailNormal={-chord.y,chord.x,0};
        const float attack=std::atan2(dot(flowDir,sailNormal),dot(flowDir,chord));
        const float liftCoefficient=1.35F*std::sin(2.F*attack);
        const float dragCoefficient=0.10F+0.70F*std::pow(std::sin(attack),2.F);
        const Float3 liftDir={-flowDir.y,flowDir.x,0};
        const float pressure=.5F*airDensity*airSpeed*airSpeed*p.sailArea;
        // A sailing vessel cannot produce forward drive with its sail furled,
        // but it remains a passive obstacle to wind while idle.
        const float exposure=.18F+.82F*std::max(0.F,throttle);
        const Float3 sailForce=multiply(add(multiply(liftDir,liftCoefficient),
             multiply(flowDir,dragCoefficient)),pressure*exposure);
        result.force=add(result.force,sailForce);
        result.torque=add(result.torque,cross3(rotate(body.orientation,{0,0,p.mastHeight}),sailForce));
        result.sailThrust=dot(sailForce,forward);
    }
    // A centerboard/keel resists water-relative sideslip. This is the
    // essential distinction between a sideways-drifting raft and a sailboat.
    if(p.keelArea>0&&submerged>0){
        const float sideForce=-.5F*waterDensity*p.keelArea*side*std::abs(side)*submerged*.85F;
        const Float3 force=multiply(right,sideForce);
        result.force=add(result.force,force);
        result.keelForce=sideForce;
        const Float3 keelCenter=rotate(body.orientation,{0,-hull.lengthMetres*.08F,-hull.draftMetres*.6F});
        result.torque=add(result.torque,cross3(keelCenter,force));
    }
    // Rudder force is ineffective without water flow. Steerage increases
    // with speed; W1's prototype torque value remains a scaling parameter.
    if(p.rudderNewtons>0&&submerged>.02F){
        const float flow=std::clamp(std::abs(along)/6.F,0.F,2.F);
        const float torque=-helm*p.rudderNewtons*flow*submerged;
        const float sideForce=helm*.5F*waterDensity*std::min(hull.beamMetres,hull.draftMetres)*
                               along*std::abs(along)*.10F*submerged;
        const Float3 finForce=multiply(right,sideForce);
        result.force=add(result.force,finForce);
        const Float3 stern=rotate(body.orientation,{0,-hull.lengthMetres*.42F,-hull.draftMetres*.3F});
        result.torque=add(result.torque,cross3(stern,finForce));
        result.torque.z+=torque;
        result.rudderMoment=torque;
    }
    // Propeller / pump-jet thrust is independent of apparent wind. A wave
    // runner's vectored pump-jet produces steering force even at low speed.
    if(p.thrustNewtons>0){
        float wet=std::clamp(submerged*2.5F,0.F,1.F);
        if(p.type==WatercraftType::WaveRunner)wet=std::clamp(submerged*3.F,0.F,1.F);
        const float jetAngle=p.type==WatercraftType::WaveRunner?helm*.44F:0.F;
        const Float3 heading=add(multiply(forward,std::cos(jetAngle)),
                                 multiply(right,std::sin(jetAngle)));
        const Float3 prop=multiply(heading,p.thrustNewtons*throttle*wet);
        result.force=add(result.force,prop);
        const Float3 aft=rotate(body.orientation,{0,-hull.lengthMetres*.38F,-hull.draftMetres*.22F});
        result.torque=add(result.torque,cross3(aft,prop));
    }
    // Planing lift is dynamic hydrodynamic pressure, not an arbitrary altitude
    // snap. It decreases when the hull becomes airborne or moves backward.
    if(p.planingFactor>0&&submerged>0&&along>2.F){
        const float area=hull.beamMetres*hull.lengthMetres*.38F;
        const float raw=.5F*waterDensity*area*along*along*p.planingFactor*submerged;
        const float lift=std::min(raw,hull.massKg*9.81F*1.2F);
        const Float3 liftForce={0,0,lift};
        result.force=add(result.force,liftForce);
        const auto at=rotate(body.orientation,{0,hull.lengthMetres*.15F,-hull.draftMetres*.4F});
        result.torque=add(result.torque,cross3(at,liftForce));
        result.planingLift=lift;
    }
    return result;
}
} // namespace dve
