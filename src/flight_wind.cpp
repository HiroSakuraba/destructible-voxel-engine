#include "dve/flight_wind.hpp"
#include <algorithm>
#include <cmath>
#include <utility>
namespace dve {
namespace {
constexpr float kEpsilon=0.0001F;
float smooth(float x)noexcept {const float t=std::clamp(x,0.0F,1.0F);return t*t*(3.0F-2.0F*t);}
bool finite(Float3 x)noexcept{return std::isfinite(x.x)&&std::isfinite(x.y)&&std::isfinite(x.z);}
bool finite(Quaternion q)noexcept{return std::isfinite(q.x)&&std::isfinite(q.y)&&std::isfinite(q.z)&&std::isfinite(q.w);}
}
bool validate_flight_wind_zone(const FlightWindZone& z)noexcept{
    const auto& p=z.transform.position;
    const auto& q=z.transform.rotation;
    const float q2=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    return z.stableId!=0&&finite(p)&&finite(q)&&std::abs(q2-1.0F)<0.02F&&
        std::abs(p.x)<1000000.0F&&std::abs(p.y)<1000000.0F&&std::abs(p.z)<1000000.0F&&
        z.kind<=FlightWindKind::Ridge&&std::isfinite(z.radiusMeters)&&
        z.radiusMeters>=5&&z.radiusMeters<=5000&&std::isfinite(z.heightMeters)&&
        z.heightMeters>=10&&z.heightMeters<=5000&&std::isfinite(z.strengthMetersPerSecond)&&
        z.strengthMetersPerSecond>=0&&z.strengthMetersPerSecond<=30&&
        std::isfinite(z.sinkFraction)&&z.sinkFraction>=0&&z.sinkFraction<=2;
}
void FlightWindField::configure(Float3 horizontalWind,float shear,float gust)noexcept{
    base_={finite(horizontalWind)?std::clamp(horizontalWind.x,-100.0F,100.0F):0.0F,
           finite(horizontalWind)?std::clamp(horizontalWind.y,-100.0F,100.0F):0.0F,0};
    shear_=std::isfinite(shear)?std::clamp(shear,-30.0F,30.0F):0;
    gust_=std::isfinite(gust)?std::clamp(gust,0.0F,20.0F):0;
}
void FlightWindField::set_zones(std::vector<FlightWindZone> zones){
    std::erase_if(zones,[](const auto& z){return !validate_flight_wind_zone(z);});
    std::sort(zones.begin(),zones.end(),[](const auto& a,const auto& b){return a.stableId<b.stableId;});
    if(zones.size()>128)zones.resize(128);
    zones_.swap(zones);
}
FlightWindSample FlightWindField::sample(Float3 p,float seconds)const noexcept{
    FlightWindSample result{};
    if(!finite(p)||!std::isfinite(seconds))return result;
    const float shearHeight=std::clamp(p.z,0.0F,5000.0F)/100.0F;
    const float directional=1.0F+0.08F*std::log1p(shearHeight);
    result.velocity={base_.x*directional+shear_*std::log1p(shearHeight),
                     base_.y*directional,0};
    const float wave=std::sin(seconds*0.65F+p.x*0.003F+p.y*0.002F);
    result.velocity.x+=gust_*wave;
    result.velocity.y+=gust_*std::sin(seconds*0.51F+p.y*0.004F)*0.5F;
    for(const auto& z:zones_){
        const Float3 relative=subtract(p,z.transform.position);
        if(z.kind==FlightWindKind::Thermal){
            // Compact core + weaker annular sink; material volumes are authorable.
            // A bounded wind drift moves the column without random state.
            const Float3 advected={relative.x-base_.x*seconds*0.08F,
                                    relative.y-base_.y*seconds*0.08F,relative.z};
            if(advected.z<0||advected.z>z.heightMeters)continue;
            const float radial=std::sqrt(advected.x*advected.x+advected.y*advected.y)/z.radiusMeters;
            if(radial>=1.8F)continue;
            const float altitude=smooth(advected.z/15.0F)*smooth((z.heightMeters-advected.z)/35.0F);
            float vertical=0;
            if(radial<1.0F){
                const float core=1.0F-radial*radial;vertical=z.strengthMetersPerSecond*core*core;
            } else {
                const float t=(radial-1.0F)/0.8F;
                vertical=-z.strengthMetersPerSecond*z.sinkFraction*4.0F*t*(1.0F-t);
            }
            const float up=vertical*altitude;
            result.thermalVertical+=up;
            result.velocity.z+=up;
        }else{
            // +Y is the authored windward normal, +X runs along a ridge.
            // Reversing the incoming wind reverses the windward/lee sides.
            const Float3 along=rotate(z.transform.rotation,{1,0,0});
            const Float3 face=rotate(z.transform.rotation,{0,1,0});
            const float x=dot(relative,along),side=dot(relative,face);
            if(std::abs(x)>z.radiusMeters||std::abs(side)>z.radiusMeters*2)continue;
            if(p.z<z.transform.position.z-z.heightMeters ||
               p.z>z.transform.position.z+z.heightMeters)continue;
            const float windwardComponent=-(result.velocity.x*face.x+result.velocity.y*face.y);
            if(std::abs(windwardComponent)<kEpsilon)continue;
            const bool windward=side*windwardComponent>0;
            const float edge=smooth((z.radiusMeters-std::abs(x))/(z.radiusMeters*0.25F));
            const float distance=std::exp(-std::abs(side)/std::max(5.0F,z.radiusMeters*0.30F));
            const float verticalEnvelope=smooth((z.transform.position.z+z.heightMeters-p.z)/z.heightMeters)*
                smooth((p.z-(z.transform.position.z-z.heightMeters))/z.heightMeters);
            const float magnitude=z.strengthMetersPerSecond*
                std::clamp(std::abs(windwardComponent)/10.0F,0.0F,2.0F)*edge*distance*verticalEnvelope;
            const float up=windward?magnitude:-magnitude*z.sinkFraction;
            result.ridgeVertical+=up;
            result.velocity.z+=up;
        }
    }
    result.backgroundVertical=result.velocity.z-result.ridgeVertical-result.thermalVertical;
    // Prevent stacked authoring zones from producing unbounded wind speeds.
    const float mag=length(result.velocity);
    if(mag>150.0F)result.velocity=multiply(result.velocity,150.0F/mag);
    return result;
}
} // namespace dve
