#include "dve/flight_aerodynamics.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
FlightAeroSample flight_aerodynamics(const FlightProfile& cfg,Float3 air,
                                    Quaternion orientation,float density)noexcept{
    FlightAeroSample result{};
    const float speed=length(air);
    if(!(speed>1.e-4F)||!std::isfinite(speed)||!std::isfinite(density)||density<=0)return result;
    const Float3 forward=rotate(orientation,{0,1,0}),up=rotate(orientation,{0,0,1});
    const Float3 airDir=multiply(air,1.0F/speed);
    const float along=dot(air,forward),vertical=dot(air,up);
    const float alpha=std::atan2(-vertical,std::max(0.01F,along));
    const float stallAngle=std::max(0.05F,cfg.stallAngle);
    const float excess=std::max(0.0F,std::abs(alpha)-stallAngle);
    // Smooth post-stall CL decay, rather than saturating lift indefinitely.
    const float stallRatio=excess/stallAngle;
    const float liftRetention=1.0F/(1.0F+4.0F*stallRatio*stallRatio);
    const float cl=std::clamp(0.36F+cfg.liftSlope*alpha,-cfg.liftLimit,cfg.liftLimit)*liftRetention;
    const float cd=std::max(0.0F,cfg.parasiteDrag)+std::max(0.0F,cfg.inducedDrag)*cl*cl+
                   0.9F*(1.0F-liftRetention);
    const float q=0.5F*density*speed*speed;
    const Float3 liftProjection=subtract(up,multiply(airDir,dot(up,airDir)));
    const float liftNorm=length(liftProjection);
    const Float3 liftDir=liftNorm>1.e-5F?multiply(liftProjection,1.0F/liftNorm):Float3{};
    const float lift=q*std::max(0.0F,cfg.wingArea)*cl;
    const float drag=q*std::max(0.0F,cfg.wingArea)*cd;
    result.forceNewton=subtract(multiply(liftDir,lift),multiply(airDir,drag));
    // Body drag is linear in air-relative speed times its magnitude, hence
    // F · v <= 0 in still air even under arbitrarily banked orientations.
    result.forceNewton=subtract(result.forceNewton,multiply(air,
        std::max(0.0F,cfg.bodyDrag)*speed));
    result.airspeed=speed;result.angleOfAttack=alpha;result.liftCoefficient=cl;
    result.dragCoefficient=cd;result.dynamicPressure=q;
    result.stallMarginRadians=stallAngle-std::abs(alpha);
    result.stalled=cfg.wingArea>0.01F&&result.stallMarginRadians<0;
    return result;
}
} // namespace dve
