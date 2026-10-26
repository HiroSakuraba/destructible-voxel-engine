#include "dve/water_wake.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
constexpr float tau=6.2831853071795864769F;
float clampf(float x,float a,float b)noexcept{return std::clamp(x,a,b);}
}
bool valid_water_wake(const WaterWakeEvent& w)noexcept{
 const float l2=w.direction.x*w.direction.x+w.direction.y*w.direction.y;
 return std::isfinite(w.position.x)&&std::isfinite(w.position.y)&&
   std::abs(w.position.x)<100000&&std::abs(w.position.y)<100000&&
   std::isfinite(l2)&&std::abs(l2-1.F)<.02F&&
   std::isfinite(w.bornSeconds)&&w.bornSeconds>=0&&w.bornSeconds<1.e9&&
   std::isfinite(w.amplitude)&&w.amplitude>0&&w.amplitude<=2.5F&&
   std::isfinite(w.wavelength)&&w.wavelength>=.8F&&w.wavelength<=25.F&&
   std::isfinite(w.width)&&w.width>=.4F&&w.width<=18.F&&
   std::isfinite(w.speed)&&w.speed>=.4F&&w.speed<=9.F&&
   std::isfinite(w.lifetime)&&w.lifetime>=.5F&&w.lifetime<=12.F;
}
bool WaterWakeField::emit(const WaterWakeEvent& w)noexcept{
 if(!valid_water_wake(w))return false;
 if(count_==MaxEvents){
   for(std::size_t i=1;i<count_;++i)events_[i-1]=events_[i];
   --count_;
 }
 events_[count_++]=w;return true;
}
bool WaterWakeField::restore(std::span<const WaterWakeEvent> list,double now)noexcept{
 if(!std::isfinite(now)||now<0||list.size()>MaxEvents)return false;
 for(const auto& e:list)
   if(!valid_water_wake(e)||e.bornSeconds>now+1.e-4||
      now-e.bornSeconds>e.lifetime+1.e-4)return false;
 count_=list.size();
 for(std::size_t i=0;i<count_;++i)events_[i]=list[i];
 return true;
}
void WaterWakeField::prune(double now)noexcept{
 if(!std::isfinite(now))return;
 std::size_t output=0;
 for(std::size_t i=0;i<count_;++i){
   const double age=now-events_[i].bornSeconds;
   if(age>=0&&age<events_[i].lifetime)events_[output++]=events_[i];
 }
 count_=output;
}
WaterWakeSample WaterWakeField::sample(float x,float y,double time)const noexcept{
 WaterWakeSample result{};
 if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(time))return result;
 for(std::size_t i=0;i<count_;++i){
   const auto& w=events_[i];
   const float age=float(time-w.bornSeconds);
   if(age<0||age>=w.lifetime)continue;
   const float dx=x-w.position.x,dy=y-w.position.y;
   const float radius2=dx*dx+dy*dy;
   const float maxR=w.speed*age+4*w.width;
   if(radius2>maxR*maxR)continue;
   const float radius=std::sqrt(radius2+1.e-6F);
   // Finite expanding ring. Forward propagation is suppressed to form an
   // approximate Kelvin-style trailing wedge rather than a uniform explosion.
   const float backwards=-(dx*w.direction.x+dy*w.direction.y)/radius;
   const float aft=.18F+.82F*clampf(.5F+.5F*backwards,0.F,1.F);
   const float dr=radius-w.speed*age;
   if(std::abs(dr)>4*w.width)continue;
   const float invWidth=1.F/w.width;
   const float gauss=std::exp(-.5F*dr*dr*invWidth*invWidth);
   const float decay=std::exp(-age*1.6F/w.lifetime);
   const float k=tau/w.wavelength;
   const float sn=std::sin(k*dr),cs=std::cos(k*dr);
   const float A=w.amplitude*decay*aft*gauss;
   const float height=A*cs;
   const float radialDerivative=A*(-dr*invWidth*invWidth*cs-k*sn);
   const float invRadius=1.F/radius;
   result.height+=height;
   result.slopeX+=radialDerivative*dx*invRadius;
   result.slopeY+=radialDerivative*dy*invRadius;
   result.verticalVelocity+=A*(w.speed*(dr*invWidth*invWidth*cs+k*sn)-
                                 1.6F/w.lifetime*cs);
   result.foam+=std::abs(height)*.75F+
                std::max(0.F,-radialDerivative)*.12F;
 }
 result.height=clampf(result.height,-2.5F,2.5F);
 result.slopeX=clampf(result.slopeX,-1.4F,1.4F);
 result.slopeY=clampf(result.slopeY,-1.4F,1.4F);
 result.verticalVelocity=clampf(result.verticalVelocity,-6.F,6.F);
 result.foam=clampf(result.foam,0.F,1.F);
 return result;
}
}
