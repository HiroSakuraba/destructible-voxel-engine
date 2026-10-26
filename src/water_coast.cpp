#include "dve/water_coast.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
constexpr float tau=6.283185307179586F;
constexpr float gravity=9.81F;
float smooth(float a,float b,float x)noexcept{
 const float u=std::clamp((x-a)/(b-a),0.F,1.F);return u*u*(3.F-2.F*u);
}
float finiteClamp(float x,float lo,float hi)noexcept{return std::clamp(x,lo,hi);}
}
bool valid_water_coast(const WaterCoast& c)noexcept{
 return std::isfinite(c.shoreline.x)&&std::abs(c.shoreline.x)<100000.F&&
  std::isfinite(c.shoreline.y)&&std::abs(c.shoreline.y)<100000.F&&
  std::isfinite(c.halfWidth)&&c.halfWidth>=15&&c.halfWidth<=3000&&
  std::isfinite(c.offshoreLength)&&c.offshoreLength>=30&&c.offshoreLength<=1500&&
  std::isfinite(c.slope)&&c.slope>=.005F&&c.slope<=.25F&&
  std::isfinite(c.swellHeight)&&c.swellHeight>=.2F&&c.swellHeight<=5&&
  std::isfinite(c.periodSeconds)&&c.periodSeconds>=3&&c.periodSeconds<=20&&
  std::isfinite(c.peel)&&std::abs(c.peel)<=.5F;
}
WaterCoastSample sample_water_coast(const WaterCoast& c,float x,float y,double time)noexcept{
 WaterCoastSample out{};
 if(!valid_water_coast(c)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(time))return out;
 const float side=std::abs(x-c.shoreline.x);
 const float offshore=c.shoreline.y-y;
 if(side>=c.halfWidth||offshore>c.offshoreLength||offshore< -20.F)return out;
 out.inZone=true;out.depth=c.slope*std::max(0.F,offshore);
 if(offshore<=0)return out;
 // Fade at the edges so the local surf can be overlaid continuously on the
 // shared open-ocean surface without a discontinuity in query height.
 const float lateralFade=1.F-smooth(c.halfWidth*.75F,c.halfWidth,side);
 const float outerFade=smooth(0.F,c.offshoreLength*.22F,c.offshoreLength-offshore);
 const float beachFade=smooth(0.F,2.5F,out.depth);
 const float depth=std::max(out.depth,.65F);
 const float shoaling=finiteClamp(std::pow(4.F/depth,.18F),.65F,2.1F);
 const float amplitude=c.swellHeight*shoaling*lateralFade*outerFade*beachFade;
 // A depth-sensitive propagation speed near the surf zone. Wave phase and
 // celerity depend on local bathymetry; this is a simplified shoaling model.
 const float velocity=std::min(7.F,std::sqrt(gravity*depth));
 const float waveLength=std::max(4.F,velocity*c.periodSeconds);
 const float k=tau/waveLength;
 const float omega=tau/c.periodSeconds;
 const double phase=std::remainder(double(k)*(y-c.shoreline.y-
           c.peel*(x-c.shoreline.x))-double(omega)*time,double(tau));
 const float sn=std::sin(phase),cs=std::cos(phase);
 // Second harmonic steepens the crests without claiming an overturning lip.
 const float shape=sn+.22F*std::sin(2*phase);
 const float deriv=cs+.44F*std::cos(2*phase);
 out.height=amplitude*shape;
 out.slopeY=amplitude*k*deriv;
 out.slopeX=-amplitude*k*c.peel*deriv;
 out.verticalVelocity=-omega*amplitude*deriv;
 // Breaking begins where local depth is small compared with swell height.
 const float criticalDepth=std::max(1.6F,1.7F*c.swellHeight);
 const float shallow=1.F-smooth(criticalDepth*.45F,criticalDepth*1.5F,out.depth);
 const float crest=smooth(.10F,.75F,sn);
 out.breaking=shallow*crest*lateralFade*outerFade*beachFade;
 out.foam=finiteClamp(out.breaking*.90F+shallow*.12F*crest,0.F,1.F);
 out.flowY=out.breaking*(1.5F+velocity*.30F); // onshore whitewater transport
 return out;
}
} // namespace dve
