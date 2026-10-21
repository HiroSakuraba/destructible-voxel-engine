#include "dve/nurbs_queries.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
namespace dve {
std::optional<NurbsLength> measure_nurbs_curve(const NurbsSource& s,double begin,double end,double tolerance,std::array<double,3> scale,std::string* error){
    auto fail=[&](const char* message)->std::optional<NurbsLength>{if(error)*error=message;return {};};
    if(!validate_nurbs(s,error))return {};
    if(!s.is_curve())return fail("Length measurement requires a NURBS curve");
    if(!std::isfinite(begin)||!std::isfinite(end)||begin>end||begin<s.u.knots[s.u.degree]||end>s.u.knots[s.columns])return fail("Length interval must be ordered inside the curve domain");
    if(!std::isfinite(tolerance)||tolerance<=0)return fail("Length tolerance must be finite and positive");
    for(double x:scale)if(!std::isfinite(x))return fail("Length metric must be finite");
    NurbsLength result;
    if(begin==end){if(error)error->clear();return result;}
    bool exhausted=false;
    // Each span is evaluated independently, including one-sided knot limits.
    auto speed=[&](double t,double lo,double hi){
        if(++result.evaluations>200000){exhausted=true;return 0.0;}
        t=std::clamp(t,std::nextafter(lo,hi),std::nextafter(hi,lo));
        auto sample=evaluate_nurbs(s,t,0,error);
        if(!sample){exhausted=true;return 0.0;}
        const double value=std::hypot(sample->du[0]*scale[0],sample->du[1]*scale[1],sample->du[2]*scale[2]);
        if(!std::isfinite(value))exhausted=true;
        return value;
    };
    std::vector<double> cuts{begin};
    for(double k:s.u.knots)if(k>begin&&k<end&&k>cuts.back())cuts.push_back(k);
    cuts.push_back(end);
    const double budget=tolerance/double((cuts.size()-1)*8);
    for(std::size_t span=1;span<cuts.size();++span){
        const double lo=cuts[span-1],hi=cuts[span];
        if(std::nextafter(lo,hi)>std::nextafter(hi,lo))return fail("NURBS length span has no representable interior");
        std::function<bool(double,double,double,double,double,double,double,unsigned)> integrate;
        integrate=[&](double a,double b,double fa,double fm,double fb,double whole,double eps,unsigned depth){
            if(exhausted)return false;
            const double m=std::midpoint(a,b),l=std::midpoint(a,m),r=std::midpoint(m,b);
            if(!(a<l&&l<m&&m<r&&r<b))return false;
            const double fl=speed(l,lo,hi),fr=speed(r,lo,hi);
            const double left=(m-a)*(fa+4*fl+fm)/6,right=(b-m)*(fm+4*fr+fb)/6;
            const double delta=left+right-whole,estimate=std::abs(delta)/15;
            if(exhausted||!std::isfinite(estimate))return false;
            if(estimate<=eps){result.length+=left+right+delta/15;result.estimatedError+=estimate;return true;}
            if(depth==20)return false;
            return integrate(a,m,fa,fl,fm,left,eps/2,depth+1)&&integrate(m,b,fm,fr,fb,right,eps/2,depth+1);
        };
        // Eight seed intervals reduce aliasing before adaptive refinement.
        for(unsigned i=0;i<8;++i){
            const double a=std::lerp(lo,hi,double(i)/8),b=std::lerp(lo,hi,double(i+1)/8),m=std::midpoint(a,b);
            const double fa=speed(a,lo,hi),fm=speed(m,lo,hi),fb=speed(b,lo,hi),whole=(b-a)*(fa+4*fm+fb)/6;
            if(!integrate(a,b,fa,fm,fb,whole,budget,0))return fail("NURBS length did not converge within numerical/work limits; increase tolerance");
        }
    }
    if(!std::isfinite(result.length)||result.length<0)return fail("Non-finite NURBS length");
    if(error)error->clear();
    return result;
}
}
