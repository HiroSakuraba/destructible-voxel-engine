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
// Orientation whose local +Z is `direction` and whose local +Y is `forward` projected onto the
// plane perpendicular to it. Falls back to the shortest-arc alignment when no usable roll
// reference exists.
Quaternion oriented_z_axis(Float3 direction,Float3 forward,Float3 fallbackForward);
}
Float3 mech_limit_ankle_normal(Float3 contactNormal,float limit) noexcept {
 if(!finite(contactNormal)||!std::isfinite(limit)||limit<0.F||limit>kPi*.5F)
  return {0,0,1};
 const Float3 n=unit(contactNormal);
 if(n.z<=0.F)return {0,0,1};
 const float radial=std::hypot(n.x,n.y);
 if(radial<1.e-6F)return {0,0,1};
 const float tilt=std::atan2(radial,n.z);
 if(tilt<=limit)return n;
 const float horizontal=std::sin(limit)/radial;
 return {n.x*horizontal,n.y*horizontal,std::cos(limit)};
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
namespace {
Quaternion oriented_z_axis(Float3 direction,Float3 forward,Float3 fallbackForward){
    const Float3 z=unit(direction);
    if(magnitude(z)<.5F)return {};
    for(const Float3 hint:{forward,fallbackForward}){
        if(!finite(hint))continue;
        const Float3 y=unit(minus(hint,times(z,dot3(hint,z))));
        if(magnitude(y)<.5F)continue;
        const Float3 x=cross(y,z); // right-handed basis (x,y,z)
        // Rotation matrix with columns x,y,z -> quaternion (Shepperd's method).
        const float m00=x.x,m01=y.x,m02=z.x,m10=x.y,m11=y.y,m12=z.y,m20=x.z,m21=y.z,m22=z.z;
        const float trace=m00+m11+m22;
        float qx,qy,qz,qw;
        if(trace>0.F){
            const float r=std::sqrt(1.F+trace)*2.F;
            qw=.25F*r;qx=(m21-m12)/r;qy=(m02-m20)/r;qz=(m10-m01)/r;
        }else if(m00>m11&&m00>m22){
            const float r=std::sqrt(1.F+m00-m11-m22)*2.F;
            qw=(m21-m12)/r;qx=.25F*r;qy=(m01+m10)/r;qz=(m02+m20)/r;
        }else if(m11>m22){
            const float r=std::sqrt(1.F+m11-m00-m22)*2.F;
            qw=(m02-m20)/r;qx=(m01+m10)/r;qy=.25F*r;qz=(m12+m21)/r;
        }else{
            const float r=std::sqrt(1.F+m22-m00-m11)*2.F;
            qw=(m10-m01)/r;qx=(m02+m20)/r;qy=(m12+m21)/r;qz=.25F*r;
        }
        const float n=std::sqrt(qx*qx+qy*qy+qz*qz+qw*qw);
        if(!(n>1.e-6F))continue;
        return Quaternion{qx/n,qy/n,qz/n,qw/n};
    }
    return mech_align_z_axis(direction);
}
}
std::optional<MechLegIkPose> solve_mech_leg_ik(
  Float3 hip, Float3 requested, Float3 pole, const MechLegIkConfig& c,
  Float3 boneForward) noexcept {
    if(!finite(hip)||!finite(requested)||!finite(pole)||
       !std::isfinite(c.thighLength)||!std::isfinite(c.shinLength)||
       !std::isfinite(c.minimumKneeBendRadians)||
       c.thighLength<.05F||c.thighLength>20.F||
       c.shinLength<.05F||c.shinLength>20.F||
       c.minimumKneeBendRadians<0||c.minimumKneeBendRadians>.8F)return std::nullopt;
    Float3 toward=minus(requested,hip);
    float distance=magnitude(toward);
    if(!std::isfinite(distance))return std::nullopt; // coordinates so large that |hip-ankle| overflows
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
    pose.thighRotation=oriented_z_axis(minus(knee,hip),boneForward,perpendicular);
    pose.shinRotation=oriented_z_axis(minus(ankle,knee),boneForward,perpendicular);
    pose.reachClamped=std::abs(distance-clamped)>1.e-5F;
    return pose;
}
} // namespace dve
