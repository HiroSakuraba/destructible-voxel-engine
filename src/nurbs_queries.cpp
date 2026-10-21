#include "dve/nurbs_queries.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
namespace dve {
namespace {
std::optional<NurbsLength> measure_curve(const NurbsSource& s,double begin,double end,double tolerance,std::array<double,3> scale,unsigned& remaining,std::string* error){
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
        if(remaining==0){exhausted=true;return 0.0;}
        --remaining;++result.evaluations;
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
std::optional<NurbsLength> measure_nurbs_curve(const NurbsSource& s,double begin,double end,double tolerance,std::array<double,3> scale,std::string* error){
    unsigned remaining=200000;
    return measure_curve(s,begin,end,tolerance,scale,remaining,error);
}
std::optional<NurbsLengthParameter> nurbs_parameter_at_length_fraction(const NurbsSource& s,double fraction,double tolerance,std::array<double,3> scale,std::string* error){
    auto fail=[&](const char* message)->std::optional<NurbsLengthParameter>{if(error)*error=message;return {};};
    if(!validate_nurbs(s,error))return {};
    if(!s.is_curve())return fail("Length inversion requires a NURBS curve");
    if(!std::isfinite(fraction)||fraction<0||fraction>1)return fail("Length fraction must be finite and inside 0..1");
    if(!std::isfinite(tolerance)||tolerance<=0||tolerance/16==0)return fail("Length inversion tolerance must be finite, positive and representable");
    const double begin=s.u.knots[s.u.degree],end=s.u.knots[s.columns];
    unsigned remaining=200000;
    auto total=measure_curve(s,begin,end,tolerance/16,scale,remaining,error);if(!total)return {};
    if(total->length<=0)return fail("A zero-length curve has no unique length parameter");
    const double rounding=16*std::numeric_limits<double>::epsilon()*total->length;
    if(tolerance<rounding)return fail("Length inversion tolerance is below numerical resolution for this metric");
    NurbsLengthParameter result;result.total=*total;
    if(fraction==0||fraction==1){
        result.parameter=fraction==0?begin:end;result.lengthBefore=fraction*total->length;
        result.evaluations=200000-remaining;if(error)error->clear();return result;
    }
    const double target=fraction*total->length;
    double lo=begin,hi=end,t=std::lerp(begin,end,fraction);
    for(unsigned iteration=0;iteration<64;++iteration){
        if(!(t>lo&&t<hi))return fail("Length inversion exhausted representable parameter resolution");
        auto prefix=measure_curve(s,begin,t,tolerance/16,scale,remaining,error);if(!prefix)return {};
        const double residual=prefix->length-target;
        result.parameter=t;result.lengthBefore=prefix->length;
        result.estimatedResidual=std::abs(residual)+prefix->estimatedError+fraction*total->estimatedError+rounding;
        result.evaluations=200000-remaining;
        if(result.estimatedResidual<=tolerance){if(error)error->clear();return result;}
        if(residual<0)lo=t;else hi=t;
        t=std::midpoint(lo,hi);
    }
    return fail("Length inversion did not converge within 64 iterations");
}
}
