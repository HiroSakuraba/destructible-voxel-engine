#include "dve/mecha_leg_ik.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
constexpr float kPi=3.14159265358979323846F;
bool finite(Float3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
Float3 plus(Float3 a,Float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 minus(Float3 a,Float3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 times(Float3 a,float k){return {a.x*k,a.y*k,a.z*k};}
float dot3(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Float3 cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float magnitude(Float3 a){return std::sqrt(dot3(a,a));}
Float3 unit(Float3 a){const auto n=magnitude(a);return n>1.e-7F?times(a,1.F/n):Float3{};}
}
Quaternion mech_align_z_axis(Float3 direction) noexcept {
    const Float3 up{0,0,1};
    if(!finite(direction))return {};
    const auto normalized=unit(direction);
    if(magnitude(normalized)<.5F)return {};
    const float cosine=std::clamp(dot3(up,normalized),-1.F,1.F);
    if(cosine>1.F-1.e-6F)return {};
    if(cosine< -1.F+1.e-6F)return quaternion_from_axis_angle({1,0,0},kPi);
    const auto axis=unit(cross(up,normalized));
    return quaternion_from_axis_angle(axis,std::acos(cosine));
}
std::optional<MechLegIkPose> solve_mech_leg_ik(
  Float3 hip, Float3 requested, Float3 pole, const MechLegIkConfig& c) noexcept {
    if(!finite(hip)||!finite(requested)||!finite(pole)||
       !std::isfinite(c.thighLength)||!std::isfinite(c.shinLength)||
       !std::isfinite(c.minimumKneeBendRadians)||
       c.thighLength<.05F||c.thighLength>20.F||
       c.shinLength<.05F||c.shinLength>20.F||
       c.minimumKneeBendRadians<0||c.minimumKneeBendRadians>.8F)return std::nullopt;
    Float3 toward=minus(requested,hip);
    float distance=magnitude(toward);
    const Float3 axis=distance>1.e-6F?times(toward,1.F/distance):Float3{0,0,-1};
    const float fullyExtended=c.thighLength+c.shinLength;
    const float maximum=std::max(.015F,fullyExtended-std::max(.001F,
           fullyExtended*(1.F-std::cos(c.minimumKneeBendRadians*.5F))));
    const float minimum=std::abs(c.thighLength-c.shinLength)+.002F;
    const float clamped=std::clamp(distance,minimum,maximum);
    const Float3 ankle=plus(hip,times(axis,clamped));
    const float along=(c.thighLength*c.thighLength-
              c.shinLength*c.shinLength+clamped*clamped)/(2.F*clamped);
    const float offset=std::sqrt(std::max(0.F,c.thighLength*c.thighLength-along*along));
    Float3 perpendicular=minus(pole,times(axis,dot3(pole,axis)));
    if(magnitude(perpendicular)<1.e-5F){
        const Float3 fallback=std::abs(axis.z)<.8F?Float3{0,0,1}:Float3{0,1,0};
        perpendicular=minus(fallback,times(axis,dot3(fallback,axis)));
    }
    perpendicular=unit(perpendicular);
    const Float3 knee=plus(plus(hip,times(axis,along)),times(perpendicular,offset));
    MechLegIkPose pose;
    pose.hip=hip;pose.knee=knee;pose.ankle=ankle;
    pose.thighCenter=times(plus(hip,knee),.5F);
    pose.shinCenter=times(plus(knee,ankle),.5F);
    pose.thighRotation=mech_align_z_axis(minus(knee,hip));
    pose.shinRotation=mech_align_z_axis(minus(ankle,knee));
    pose.reachClamped=std::abs(distance-clamped)>1.e-5F;
    return pose;
}
} // namespace dve
