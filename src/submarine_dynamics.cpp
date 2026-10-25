#include "dve/submarine_dynamics.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
bool valid_submarine_config(const SubmarineConfig& c)noexcept{
 return std::isfinite(c.dryMassKg)&&c.dryMassKg>=100&&c.dryMassKg<=200000&&
  std::isfinite(c.displacementM3)&&c.displacementM3>=.2F&&c.displacementM3<=250.F&&
  std::isfinite(c.ballastCapacityKg)&&c.ballastCapacityKg>=0&&c.ballastCapacityKg<=20000&&
  std::isfinite(c.pumpRateKgPerSecond)&&c.pumpRateKgPerSecond>=0&&c.pumpRateKgPerSecond<=500&&
  std::isfinite(c.forwardThrustN)&&c.forwardThrustN>=0&&c.forwardThrustN<=1000000&&
  std::isfinite(c.verticalThrustN)&&c.verticalThrustN>=0&&c.verticalThrustN<=1000000&&
  std::isfinite(c.lateralThrustN)&&c.lateralThrustN>=0&&c.lateralThrustN<=1000000&&
  std::isfinite(c.yawTorqueNm)&&c.yawTorqueNm>=0&&c.yawTorqueNm<=1000000&&
  std::isfinite(c.pitchTorqueNm)&&c.pitchTorqueNm>=0&&c.pitchTorqueNm<=1000000&&
  std::isfinite(c.depthRatingM)&&c.depthRatingM>=5&&c.depthRatingM<=5000&&
  std::isfinite(c.halfHeightM)&&c.halfHeightM>=.2F&&c.halfHeightM<=10.F;
}
SubmarineLoads evaluate_submarine(const SubmarineConfig& c,SubmarineState& state,
  const WaterRigidState& body,const WaterVolumeSample& water,
  const SubmarineInput& input,float dt)noexcept{
 SubmarineLoads out{};
 if(!valid_submarine_config(c)||!std::isfinite(dt)||dt<=0||dt>1.F/30.F||
    !std::isfinite(state.ballastKg)||!std::isfinite(state.battery)||!std::isfinite(state.targetDepthM))
    return out;
 const float axis=std::clamp(input.ballast,-1.F,1.F);
 const bool powered=state.battery>0.0001F;
 if(powered)state.ballastKg=std::clamp(state.ballastKg+axis*c.pumpRateKgPerSecond*dt,
                                 0.F,c.ballastCapacityKg);
 const float mass=c.dryMassKg+state.ballastKg;
 // Smoothing close to the surface approximates gradual hull submersion.
 const float fraction=std::clamp((water.surfaceHeight-body.position.z+c.halfHeightM)/
                                  (2.F*c.halfHeightM),0.F,1.F);
 out.submergedFraction=fraction;out.depth=water.depthBelowSurface;
 out.pressurePascal=water.pressurePascal;
 out.ratedDepthExceeded=out.depth>c.depthRatingM;
 const float buoyancy=water.density*c.displacementM3*9.81F*fraction;
 out.forceNewton={0,0,buoyancy-mass*9.81F};
 const Float3 forward=rotate(body.orientation,{0,1,0});
 const Float3 right=rotate(body.orientation,{1,0,0});
 const Float3 up=rotate(body.orientation,{0,0,1});
 const float fwd=std::clamp(input.forward,-1.F,1.F);
 const float strafe=std::clamp(input.strafe,-1.F,1.F);
 float heave=std::clamp(input.heave,-1.F,1.F);
 if(state.depthHold&&powered&&fraction>.5F){
    const float error=std::clamp(out.depth-state.targetDepthM,-25.F,25.F);
    const float desired=mass*(2.0F*error-2.8F*body.linearVelocity.z)+
                        (mass*9.81F-buoyancy);
    // Manual heave trims the hold set-point through the *controller*.
    heave=std::clamp(desired/std::max(1.F,c.verticalThrustN)+heave*.4F,-1.F,1.F);
 }
 if(powered){
    out.forceNewton=add(out.forceNewton,multiply(forward,c.forwardThrustN*fwd*fraction));
    out.forceNewton=add(out.forceNewton,multiply(right,c.lateralThrustN*strafe*fraction));
    out.verticalThrust=c.verticalThrustN*heave*fraction;
    out.forceNewton.z+=out.verticalThrust;
    out.torqueNewtonMetres=add(multiply(up,std::clamp(input.yaw,-1.F,1.F)*c.yawTorqueNm*fraction),
                              multiply(right,std::clamp(input.pitch,-1.F,1.F)*c.pitchTorqueNm*fraction));
 }
 // Water-relative anisotropic drag: forward travel is efficient; broadside,
 // heave and angular motion are strongly damped. Above water, use light air drag.
 const Float3 relative=subtract(body.linearVelocity,water.flowVelocity);
 const float vforward=dot(relative,forward),vright=dot(relative,right),vup=dot(relative,up);
 Float3 drag=add(multiply(forward,-vforward*std::abs(vforward)*170.F),
                add(multiply(right,-vright*std::abs(vright)*900.F),
                    multiply(up,-vup*std::abs(vup)*1500.F)));
 drag=add(drag,multiply(relative,-85.F));
 out.forceNewton=add(out.forceNewton,multiply(drag,fraction));
 out.forceNewton=subtract(out.forceNewton,multiply(body.linearVelocity,(1.F-fraction)*12.F));
 out.torqueNewtonMetres=subtract(out.torqueNewtonMetres,multiply(body.angularVelocity,700.F*fraction));
 const float electrical=(std::abs(fwd)*.000025F+std::abs(strafe)*.000018F+
      std::abs(heave)*.000035F+std::abs(input.yaw)*.000012F+
      std::abs(axis)*.00002F)*dt;
 if(powered)state.battery=std::clamp(state.battery-electrical,0.F,1.F);
 out.batteryDraw=electrical;
 return out;
}
} // namespace dve
