#include "dve/nurbs_construction.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
namespace dve {
namespace {
using V=std::array<double,3>;
V add(V a,V b){for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
V sub(V a,V b){for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
V mul(V a,double s){for(auto& x:a)x*=s;return a;}
double dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
V cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double norm(V a){return std::hypot(a[0],a[1],a[2]);}
V pos(NurbsPoint p){return {p.x,p.y,p.z};}
std::optional<NurbsSource> fail(std::string* e,const char* text){if(e)*e=text;return std::nullopt;}
bool curve(const NurbsSource& s,std::string* e){if(!validate_nurbs(s,e))return false;if(!s.is_curve()){if(e)*e="Construction requires NURBS curves";return false;}return true;}
void normalize_axis(NurbsSource& s){
    const auto a=s.u.knots[s.u.degree],b=s.u.knots[s.columns];
    for(auto& t:s.u.knots)t=(t-a)/(b-a);
    double w=0;for(auto p:s.controls)w=(std::max)(w,p.weight);
    for(auto& p:s.controls)p.weight/=w;
}
NurbsAxis linear_axis(unsigned n){NurbsAxis a{1,{0,0}};for(unsigned i=1;i<n-1;++i)a.knots.push_back(double(i)/(n-1));a.knots.insert(a.knots.end(),2,1);return a;}
V rotate_axis(V p,V axis,double angle){return add(add(mul(p,std::cos(angle)),mul(cross(axis,p),std::sin(angle))),mul(axis,dot(axis,p)*(1-std::cos(angle))));}
}
std::optional<NurbsSource> reverse_nurbs_curve(const NurbsSource& s,std::string* e){
    if(!curve(s,e))return std::nullopt;
    auto r=s;std::reverse(r.controls.begin(),r.controls.end());
    const double a=s.u.knots.front(),b=s.u.knots.back();
    for(std::size_t i=0;i<r.u.knots.size();++i)r.u.knots[i]=a+(b-s.u.knots[r.u.knots.size()-1-i]);
    if(!validate_nurbs(r,e))return std::nullopt;
    return r;
}
std::optional<NurbsSource> shift_nurbs_seam(const NurbsSource& s,std::string* e){
    if(!curve(s,e))return std::nullopt;
    const auto p=s.u.degree,n=s.columns-1;
    if(n%p||n/p<2||norm(sub(pos(s.controls.front()),pos(s.controls.back())))>1e-10||
       std::abs(s.controls.front().weight-s.controls.back().weight)>1e-12*(std::max)(s.controls.front().weight,s.controls.back().weight))
        return fail(e,"Seam shift requires a closed uniform piecewise-Bezier curve");
    auto r=s;normalize_axis(r);const unsigned spans=n/p;
    std::vector<double> knots(p+1,0);
    for(unsigned j=1;j<spans;++j)knots.insert(knots.end(),p,double(j)/spans);
    knots.insert(knots.end(),p+1,1);
    for(std::size_t i=0;i<knots.size();++i)if(std::abs(knots[i]-r.u.knots[i])>1e-12)return fail(e,"Seam shift requires uniform Bezier spans; refine or rebuild first");
    r=s;std::rotate(r.controls.begin(),r.controls.begin()+p,r.controls.end()-1);r.controls.back()=r.controls.front();return r;
}
std::optional<NurbsSource> loft_nurbs(std::span<const NurbsSource> profiles,std::string* e){
    if(profiles.size()<2||profiles.size()>64)return fail(e,"Loft requires 2..64 ordered profiles");
    std::vector<NurbsSource> curves;std::map<double,unsigned> unionKnots;std::vector<double> knotReps{0.0,1.0};
    for(const auto& s:profiles){
        if(!curve(s,e))return std::nullopt;
        if(s.u.degree!=profiles.front().u.degree)return fail(e,"Loft profiles need equal degrees; degree elevation is not available");
        curves.push_back(s);normalize_axis(curves.back());
        // Normalization can turn one shared knot into neighbouring doubles (0.1 vs 0.09999999999999998);
        // snap each knot onto an already-seen representative so no artificial near-zero span appears.
        for(auto& t:curves.back().u.knots){
            auto near=std::find_if(knotReps.begin(),knotReps.end(),[&](double r){return std::abs(r-t)<=1e-10;});
            if(near!=knotReps.end())t=*near;else knotReps.push_back(t);
        }
        if(!validate_nurbs(curves.back(),e))return std::nullopt;
        std::map<double,unsigned> counts;for(auto t:curves.back().u.knots)++counts[t];
        for(auto [t,n]:counts)unionKnots[t]=(std::max)(unionKnots[t],n);
    }
    std::size_t knotCount=0;for(auto [t,n]:unionKnots){(void)t;knotCount+=n;}
    if((knotCount-profiles.front().u.degree-1)*profiles.size()>4096)return fail(e,"Refined loft exceeds the 4096-control limit");
    for(auto& s:curves)for(auto [t,n]:unionKnots){
        if(t==0||t==1)continue;
        auto count=unsigned(std::count(s.u.knots.begin(),s.u.knots.end(),t));
        for(;count<n;++count){auto r=insert_nurbs_knot(s,false,t,e);if(!r)return std::nullopt;s=std::move(*r);}
    }
    NurbsSource out=curves.front();out.rows=unsigned(curves.size());out.creaseNormals=true;out.textureV={0,1};out.v=linear_axis(out.rows);out.controls.clear();
    for(auto& s:curves){out.samplesPerSpan=(std::max)(out.samplesPerSpan,s.samplesPerSpan);out.controls.insert(out.controls.end(),s.controls.begin(),s.controls.end());}
    if(!validate_nurbs(out,e))return std::nullopt;
    return out;
}
std::optional<NurbsSource> sweep_nurbs(const NurbsSource& profile,const NurbsSource& rail,const NurbsSweepOptions& o,std::string* e){
    if(!curve(profile,e)||!curve(rail,e))return std::nullopt;
    if(o.sections<2||o.sections>64||std::size_t(o.sections)*profile.columns>4096||
       !std::isfinite(o.twistRadians)||std::abs(o.twistRadians)>64*6.283185307179586||!std::isfinite(o.endScale)||o.endScale<1e-6||o.endScale>1e6||
       !std::all_of(o.profileOrigin.begin(),o.profileOrigin.end(),[](double x){return std::isfinite(x);}))
        return fail(e,"Sweep needs 2..64 sections, positive bounded scale and finite bounded twist/origin");
    const auto a=rail.u.knots[rail.u.degree],b=rail.u.knots[rail.columns];
    auto start=evaluate_nurbs(rail,a),end=evaluate_nurbs(rail,b);
    if(!start||!end)return fail(e,"Sweep rail endpoints cannot be evaluated");
    if(norm(sub(start->position,end->position))<=1e-9)return fail(e,"Closed or coincident-end rails are not supported by this sweep");
    const double domain=b-a;const double speed=norm(start->du);if(speed*domain<=1e-12)return fail(e,"Sweep rail has a stationary start tangent");
    V previous=mul(start->du,1/speed);std::vector<V> offsets;
    for(auto p:profile.controls){auto q=sub(pos(p),o.profileOrigin);if(std::abs(dot(q,previous))>1e-6*(std::max)(1.0,norm(q)))return fail(e,"Profile must lie in a plane normal to the rail start tangent through its object origin");offsets.push_back(q);}
    NurbsSource out=profile;out.rows=o.sections;out.creaseNormals=true;out.textureV={0,1};out.v=linear_axis(o.sections);out.controls.clear();
    for(unsigned j=0;j<o.sections;++j){
        const double f=double(j)/(o.sections-1);auto sample=evaluate_nurbs(rail,std::lerp(a,b,f));
        if(!sample||norm(sample->du)*domain<=1e-12)return fail(e,"Sweep rail has a stationary sampled tangent");
        auto tangent=mul(sample->du,1/norm(sample->du));const double c=std::clamp(dot(previous,tangent),-1.0,1.0);
        if(c<-.999999)return fail(e,"Sweep rail reverses tangent; split the rail or add sections");
        auto axis=cross(previous,tangent);const double sine=norm(axis);
        if(sine>1e-12)for(auto& offset:offsets)offset=rotate_axis(offset,mul(axis,1/sine),std::atan2(sine,c));
        for(unsigned i=0;i<profile.columns;++i){auto q=add(sample->position,mul(rotate_axis(offsets[i],tangent,f*o.twistRadians),std::lerp(1.0,o.endScale,f)));out.controls.push_back({q[0],q[1],q[2],profile.controls[i].weight});}
        previous=tangent;
    }
    if(!validate_nurbs(out,e))return std::nullopt;
    return out;
}
std::optional<NurbsSource> join_nurbs_curves(std::span<const NurbsSource> curves,std::string* e){
    if(curves.size()<2||curves.size()>64)return fail(e,"Join requires 2..64 ordered curve snapshots");
    std::size_t controls=1;const auto degree=curves.front().u.degree;
    for(std::size_t i=0;i<curves.size();++i){
        const auto& s=curves[i];
        if(!curve(s,e))return {};
        if(s.u.degree!=degree)return fail(e,"Joined curves must have equal degrees; degree elevation is not available");
        controls+=s.columns-1;
        if(i){const auto a=curves[i-1].controls.back(),b=s.controls.front();
            if(a.x!=b.x||a.y!=b.y||a.z!=b.z)return fail(e,"Curve endpoints must match exactly; position and orient sources before taking snapshots");
        }
    }
    if(controls>4096)return fail(e,"Joined curve exceeds the 4096-control budget");
    NurbsSource out;out.columns=unsigned(controls);out.u.degree=degree;out.u.knots.assign(degree+1,0);out.samplesPerSpan=1;
    std::vector<double> logWeights;logWeights.reserve(controls);out.controls.reserve(controls);
    double startLog=0;
    for(std::size_t i=0;i<curves.size();++i){
        const auto& s=curves[i];const double firstWeight=s.controls.front().weight;
        const double lo=s.u.knots[degree],hi=s.u.knots[s.columns];
        for(unsigned k=degree+1;k<s.columns;++k)out.u.knots.push_back(double(i)+(s.u.knots[k]-lo)/(hi-lo));
        out.u.knots.insert(out.u.knots.end(),i+1==curves.size()?degree+1:degree,double(i+1));
        for(unsigned k=i?1:0;k<s.columns;++k){
            out.controls.push_back(s.controls[k]);logWeights.push_back(std::log(s.controls[k].weight/firstWeight)+startLog);
        }
        startLog=std::log(s.controls.back().weight/firstWeight)+startLog;
        out.samplesPerSpan=(std::max)(out.samplesPerSpan,s.samplesPerSpan);
    }
    const auto [minLog,maxLog]=std::minmax_element(logWeights.begin(),logWeights.end());
    if(*maxLog-*minLog> -std::log(1e-12))return fail(e,"Combined curve weights exceed the supported conditioning range");
    for(std::size_t i=0;i<out.controls.size();++i)out.controls[i].weight=std::exp(logWeights[i]-*maxLog);
    if(!validate_nurbs(out,e))return {};
    return out;
}

}
