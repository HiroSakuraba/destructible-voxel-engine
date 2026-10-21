#include "dve/nurbs_modeling.hpp"
#include "dve/nurbs_topology.hpp"
#include "nurbs_evaluation_internal.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <cmath>
#include <limits>
#include <numeric>
namespace dve {
namespace {
using V=std::array<double,3>;
V add(V a,V b){for(unsigned k=0;k<3;++k)a[k]+=b[k];return a;}
V sub(V a,V b){for(unsigned k=0;k<3;++k)a[k]-=b[k];return a;}
V mul(V a,double f){for(auto& x:a)x*=f;return a;}
double dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
V cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double norm(V a){return std::hypot(a[0],a[1],a[2]);}
V point(NurbsPoint p){return {p.x,p.y,p.z};}
template<class T> std::optional<T> fail(std::string* e,const char* message){if(e)*e=message;return {};}
bool tolerance_ok(double t){return std::isfinite(t)&&t>0&&t<=1e8;}
NurbsAxis uniform_axis(unsigned n,unsigned p){NurbsAxis a;a.degree=p;a.knots.assign(p+1,0);for(unsigned i=1;i<n-p;++i)a.knots.push_back(double(i)/(n-p));a.knots.insert(a.knots.end(),p+1,1);return a;}
std::vector<double> basis(const NurbsAxis& a,unsigned n,double t){
    std::vector<double> b(n+a.degree,0);if(t==a.knots[n])b[n-1]=1;else for(unsigned i=0;i<n;++i)b[i]=t>=a.knots[i]&&t<a.knots[i+1]?1:0;
    for(unsigned p=1;p<=a.degree;++p)for(unsigned i=0;i<n+a.degree-p;++i){const double l=a.knots[i+p]-a.knots[i],r=a.knots[i+p+1]-a.knots[i+1];b[i]=(l>0?(t-a.knots[i])/l*b[i]:0)+(r>0?(a.knots[i+p+1]-t)/r*b[i+1]:0);}
    b.resize(n);return b;
}
}
std::optional<NurbsSource> smooth_loft_nurbs(std::span<const NurbsSource> profiles,std::string* e){
    auto ruled=loft_nurbs(profiles,e);if(!ruled)return {};
    const unsigned n=ruled->rows,rows=3*(n-1)+1,cols=ruled->columns;
    if(std::size_t(rows)*cols>4096)return fail<NurbsSource>(e,"Smooth loft exceeds 4096 controls");
    auto out=*ruled;out.rows=rows;out.v={3,{0,0,0,0}};
    for(unsigned j=1;j<n-1;++j)out.v.knots.insert(out.v.knots.end(),3,double(j));out.v.knots.insert(out.v.knots.end(),4,double(n-1));out.controls.resize(std::size_t(rows)*cols);
    for(unsigned i=0;i<cols;++i){
        std::vector<std::array<double,4>> h(n),m(n);for(unsigned j=0;j<n;++j){auto p=ruled->controls[j*cols+i];h[j]={p.x*p.weight,p.y*p.weight,p.z*p.weight,p.weight};}
        for(unsigned k=0;k<4;++k){
            std::vector<double> diagonal(n,4),rhs(n);diagonal.front()=diagonal.back()=2;
            rhs[0]=3*(h[1][k]-h[0][k]);rhs[n-1]=3*(h[n-1][k]-h[n-2][k]);
            for(unsigned j=1;j+1<n;++j)rhs[j]=3*(h[j+1][k]-h[j-1][k]);
            for(unsigned j=1;j<n;++j){const double f=1/diagonal[j-1];diagonal[j]-=f;rhs[j]-=f*rhs[j-1];}
            m[n-1][k]=rhs[n-1]/diagonal[n-1];for(unsigned j=n-1;j-->0;)m[j][k]=(rhs[j]-m[j+1][k])/diagonal[j];
        }
        for(unsigned j=0;j<n-1;++j)for(unsigned b=0;b<4;++b){
            auto q=b<2?h[j]:h[j+1];if(b==1)for(unsigned k=0;k<4;++k)q[k]+=m[j][k]/3;if(b==2)for(unsigned k=0;k<4;++k)q[k]-=m[j+1][k]/3;
            if(!(q[3]>0)||!std::isfinite(q[3]))return fail<NurbsSource>(e,"Smooth interpolation produced nonpositive weights; use ruled loft or change sections");
            out.controls[(j*3+b)*cols+i]={q[0]/q[3],q[1]/q[3],q[2]/q[3],q[3]};
        }
    }
    if(!validate_nurbs(out,e))return {};return out;
}
std::optional<NurbsFit> fit_nurbs_curve(std::span<const V> samples,unsigned count,double tolerance,std::string* e){
    if(count<2||count>64||samples.size()<count||samples.size()>513||!tolerance_ok(tolerance))return fail<NurbsFit>(e,"Fit requires 2..64 controls, control-count..513 samples and positive finite tolerance");
    for(auto p:samples)for(double x:p)if(!std::isfinite(x)||std::abs(x)>1e8)return fail<NurbsFit>(e,"Fit points must be finite and bounded");
    NurbsSource out;out.columns=count;out.u=uniform_axis(count,(std::min)(3U,count-1));out.controls.resize(count);
    auto put=[&](unsigned i,V p){out.controls[i]={p[0],p[1],p[2],1};};put(0,samples.front());put(count-1,samples.back());
    const unsigned unknown=count-2,rows=unsigned(samples.size());
    std::vector<std::vector<double>> q(unknown,std::vector<double>(rows)),r(unknown,std::vector<double>(unknown));std::vector<V> rhs(rows);
    for(unsigned j=0;j<rows;++j){auto b=basis(out.u,count,double(j)/(rows-1));rhs[j]=sub(samples[j],add(mul(samples.front(),b.front()),mul(samples.back(),b.back())));for(unsigned i=0;i<unknown;++i)q[i][j]=b[i+1];}
    // Modified Gram-Schmidt with a second orthogonalization pass, no normal equations.
    for(unsigned i=0;i<unknown;++i){
        for(unsigned pass=0;pass<2;++pass)for(unsigned k=0;k<i;++k){double c=std::inner_product(q[k].begin(),q[k].end(),q[i].begin(),0.0);r[k][i]+=c;for(unsigned j=0;j<rows;++j)q[i][j]-=c*q[k][j];}
        r[i][i]=std::sqrt(std::inner_product(q[i].begin(),q[i].end(),q[i].begin(),0.0));if(r[i][i]<1e-10)return fail<NurbsFit>(e,"Fit matrix is rank deficient");for(auto& x:q[i])x/=r[i][i];
    }
    std::vector<V> solution(unknown);
    for(unsigned i=unknown;i-->0;){V b{};for(unsigned j=0;j<rows;++j)b=add(b,mul(rhs[j],q[i][j]));for(unsigned k=i+1;k<unknown;++k)b=sub(b,mul(solution[k],r[i][k]));solution[i]=mul(b,1/r[i][i]);put(i+1,solution[i]);}
    if(!validate_nurbs(out,e))return {};
    double error=0;for(unsigned j=0;j<rows;++j){auto p=evaluate_nurbs(out,double(j)/(rows-1));if(!p)return fail<NurbsFit>(e,"Fit evaluation failed");error=(std::max)(error,norm(sub(p->position,samples[j])));}
    if(error>tolerance)return fail<NurbsFit>(e,"Fit exceeds the requested sampled error tolerance; increase controls or tolerance");
    return NurbsFit{std::move(out),error};
}
std::optional<NurbsFit> rebuild_nurbs_curve(const NurbsSource& source,unsigned count,double tolerance,std::string* e){
    if(!validate_nurbs(source,e))return {};if(!source.is_curve())return fail<NurbsFit>(e,"Rebuild requires a curve");
    const double lo=source.u.knots.front(),hi=source.u.knots.back();std::vector<V> samples;
    for(unsigned i=0;i<257;++i){auto p=evaluate_nurbs(source,std::lerp(lo,hi,double(i)/256),0,e);if(!p)return {};samples.push_back(p->position);}
    auto fit=fit_nurbs_curve(samples,count,tolerance,e);if(!fit)return {};
    auto& out=fit->source;for(auto& t:out.u.knots)t=std::lerp(lo,hi,t);out.textureU=source.textureU;out.textureV=source.textureV;out.samplesPerSpan=source.samplesPerSpan;out.creaseNormals=source.creaseNormals;
    for(unsigned i=0;i<513;++i){const double t=std::lerp(lo,hi,double(i)/512);auto a=evaluate_nurbs(source,t),b=evaluate_nurbs(out,t);if(!a||!b)return fail<NurbsFit>(e,"Rebuild validation evaluation failed");fit->maxSampledError=(std::max)(fit->maxSampledError,norm(sub(a->position,b->position)));}
    if(fit->maxSampledError>tolerance)return fail<NurbsFit>(e,"Rebuild exceeds independent sampled error tolerance");return fit;
}
std::optional<NurbsSource> attach_nurbs_u(const NurbsSource& reference,const NurbsSource& moving,bool c1,std::string* e){
    if(!validate_nurbs(reference,e)||!validate_nurbs(moving,e))return {};
    if(reference.is_curve()||moving.is_curve()||reference.rows!=moving.rows||reference.v!=moving.v)return fail<NurbsSource>(e,"Attach requires surfaces with identical V degree, knots and row count; orient/refine first");
    if(c1&&moving.columns<3)return fail<NurbsSource>(e,"C1 attachment needs at least three U controls on the moving patch so its far edge is kept; elevate or insert a U knot first");
    auto out=moving;const double ratio=double(reference.u.degree)*(moving.u.knots[moving.u.degree+1]-moving.u.knots.front())/(double(moving.u.degree)*(reference.u.knots.back()-reference.u.knots[reference.columns-1]));
    double referenceScale=0,movingScale=0;for(auto p:reference.controls)referenceScale=(std::max)(referenceScale,p.weight);for(auto p:moving.controls)movingScale=(std::max)(movingScale,p.weight);
    for(auto& p:out.controls)p.weight/=movingScale;
    for(unsigned j=0;j<out.rows;++j){auto edge=reference.controls[j*reference.columns+reference.columns-1],prev=reference.controls[j*reference.columns+reference.columns-2];edge.weight/=referenceScale;prev.weight/=referenceScale;out.controls[j*out.columns]=edge;
        if(c1){const double w=edge.weight+ratio*(edge.weight-prev.weight);if(!(w>0)||!std::isfinite(w))return fail<NurbsSource>(e,"C1 attachment produced nonpositive homogeneous weights");V q=mul(sub(mul(point(edge),1+ratio),mul(point(prev),ratio*prev.weight/edge.weight)),edge.weight/w);out.controls[j*out.columns+1]={q[0],q[1],q[2],w};}
    }
    if(!validate_nurbs(out,e))return {};return out;
}
std::optional<NurbsProjection> project_nurbs_point(const NurbsSource& s,V target,double tolerance,std::string* e){
    if(!validate_nurbs(s,e))return {};if(s.is_curve()||!tolerance_ok(tolerance))return fail<NurbsProjection>(e,"Projection requires a surface and positive tolerance");
    for(double x:target)if(!std::isfinite(x)||std::abs(x)>1e8)return fail<NurbsProjection>(e,"Projection point is nonfinite or out of bounds");
    double weightScale=0;for(auto p:s.controls)weightScale=(std::max)(weightScale,p.weight);
    auto evaluate=[&](double u,double v){return detail::evaluate_validated_nurbs(s,u,v,weightScale);};
    const double u0=s.u.knots.front(),u1=s.u.knots.back(),v0=s.v.knots.front(),v1=s.v.knots.back();
    struct Seed{double u,v,d;};std::vector<Seed> seeds;
    for(unsigned j=0;j<=8;++j)for(unsigned i=0;i<=8;++i){const double u=double(i)/8,v=double(j)/8;auto p=evaluate(std::lerp(u0,u1,u),std::lerp(v0,v1,v));if(!p)return fail<NurbsProjection>(e,"Projection seed evaluation failed");seeds.push_back({u,v,norm(sub(p->position,target))});}
    std::sort(seeds.begin(),seeds.end(),[](auto a,auto b){return a.d<b.d;});std::optional<NurbsProjection> best;
    for(unsigned seed=0;seed<4;++seed){double u=seeds[seed].u,v=seeds[seed].v;
        for(unsigned iteration=0;iteration<40;++iteration){auto p=evaluate(std::lerp(u0,u1,u),std::lerp(v0,v1,v));if(!p)break;auto residual=sub(p->position,target),du=mul(p->du,u1-u0),dv=mul(p->dv,v1-v0);
            const double a=dot(du,du),b=dot(du,dv),c=dot(dv,dv),x=dot(du,residual),y=dot(dv,residual),det=a*c-b*b;
            if(!(a>0&&c>0&&det>1e-14*a*c))break;
            const double stepU=(c*x-b*y)/det,stepV=(a*y-b*x)/det;
            const double projectedU=std::clamp(u-stepU,0.0,1.0),projectedV=std::clamp(v-stepV,0.0,1.0);
            if(norm(add(mul(du,projectedU-u),mul(dv,projectedV-v)))<=tolerance){NurbsProjection q{std::lerp(u0,u1,u),std::lerp(v0,v1,v),norm(residual),p->position};if(!best||q.distance<best->distance)best=q;break;}
            bool improved=false;const double distance=norm(residual);
            for(unsigned line=0;line<16;++line){const double f=std::ldexp(1.0,-int(line)),nu=std::clamp(u-f*stepU,0.0,1.0),nv=std::clamp(v-f*stepV,0.0,1.0);auto q=evaluate(std::lerp(u0,u1,nu),std::lerp(v0,v1,nv));if(q&&norm(sub(q->position,target))<distance){u=nu;v=nv;improved=true;break;}}
            if(!improved)break;
        }
    }
    if(!best)return fail<NurbsProjection>(e,"Numerical projection did not converge; change geometry or tolerance");return best;
}
std::optional<NurbsSource> project_nurbs_curve(const NurbsSource& curve,const NurbsSource& surface,unsigned samples,std::string* e){
    if(!validate_nurbs(curve,e)||!curve.is_curve()||samples<2||samples>257)return fail<NurbsSource>(e,"Projection guide requires a curve and 2..257 samples");
    NurbsSource out;out.columns=samples;out.u=uniform_axis(samples,1);out.samplesPerSpan=1;
    for(unsigned i=0;i<samples;++i){auto p=evaluate_nurbs(curve,std::lerp(curve.u.knots.front(),curve.u.knots.back(),double(i)/(samples-1)));if(!p)return {};auto q=project_nurbs_point(surface,p->position,1e-6,e);if(!q)return {};out.controls.push_back({q->position[0],q->position[1],q->position[2],1});}
    if(!validate_nurbs(out,e))return {};return out;
}
namespace {
struct Plane { V origin,a,b,n; };
std::optional<Plane> affine_plane(const NurbsSource& s,double tolerance){
    if(s.is_curve())return {};Plane p{point(s.controls.front()),sub(point(s.controls[s.columns-1]),point(s.controls.front())),sub(point(s.controls[(s.rows-1)*s.columns]),point(s.controls.front())),{}};p.n=cross(p.a,p.b);if(norm(p.n)<=1e-12*norm(p.a)*norm(p.b))return {};
    for(unsigned j=0;j<s.rows;++j)for(unsigned i=0;i<s.columns;++i){double u=0,v=0;for(unsigned k=1;k<=s.u.degree;++k)u+=s.u.knots[i+k]/s.u.degree;for(unsigned k=1;k<=s.v.degree;++k)v+=s.v.knots[j+k]/s.v.degree;u=(u-s.u.knots.front())/(s.u.knots.back()-s.u.knots.front());v=(v-s.v.knots.front())/(s.v.knots.back()-s.v.knots.front());const auto q=s.controls[j*s.columns+i];if(std::abs(q.weight/s.controls.front().weight-1)>1e-12||norm(sub(point(q),add(p.origin,add(mul(p.a,u),mul(p.b,v)))))>tolerance)return {};}
    return p;
}
std::array<double,2> plane_uv(const Plane& p,V q){const double a=dot(p.a,p.a),b=dot(p.a,p.b),c=dot(p.b,p.b),det=a*c-b*b;auto d=sub(q,p.origin);return {(c*dot(d,p.a)-b*dot(d,p.b))/det,(a*dot(d,p.b)-b*dot(d,p.a))/det};}
}
std::optional<NurbsSource> intersect_nurbs_planes(const NurbsSource& a,const NurbsSource& b,double tolerance,std::string* e){
    if(!validate_nurbs(a,e)||!validate_nurbs(b,e))return {};if(!tolerance_ok(tolerance))return fail<NurbsSource>(e,"Intersection needs positive finite tolerance");
    auto p=affine_plane(a,tolerance),q=affine_plane(b,tolerance);if(!p||!q)return fail<NurbsSource>(e,"This intersection tool supports affine planar patches only");
    auto d=cross(p->n,q->n);const double dd=dot(d,d);if(dd<=1e-20*dot(p->n,p->n)*dot(q->n,q->n))return fail<NurbsSource>(e,"Parallel or coplanar patches have no unique segment intersection");
    auto origin=mul(add(mul(cross(q->n,d),dot(p->n,p->origin)),mul(cross(d,p->n),dot(q->n,q->origin))),1/dd);d=mul(d,1/std::sqrt(dd));double lo=-std::numeric_limits<double>::infinity(),hi=-lo;
    for(auto plane:{*p,*q}){auto uv=plane_uv(plane,origin),next=plane_uv(plane,add(origin,d));for(unsigned k=0;k<2;++k){const double direction=next[k]-uv[k];if(std::abs(direction)<1e-14){if(uv[k]<0||uv[k]>1)return fail<NurbsSource>(e,"Planar patches do not intersect");continue;}double x=-uv[k]/direction,y=(1-uv[k])/direction;if(x>y)std::swap(x,y);lo=(std::max)(lo,x);hi=(std::min)(hi,y);}}
    if(!std::isfinite(lo)||!std::isfinite(hi)||hi-lo<=tolerance)return fail<NurbsSource>(e,"No nondegenerate intersection segment inside both patches");
    auto x=add(origin,mul(d,lo)),y=add(origin,mul(d,hi));NurbsSource out;out.columns=2;out.u=uniform_axis(2,1);out.controls={{x[0],x[1],x[2],1},{y[0],y[1],y[2],1}};if(!validate_nurbs(out,e))return {};return out;
}
std::optional<NurbsSource> crop_nurbs_surface(const NurbsSource& s,double u0,double u1,double v0,double v1,std::string* e){
    if(!validate_nurbs(s,e))return {};if(s.is_curve()||!std::isfinite(u0)||!std::isfinite(u1)||!std::isfinite(v0)||!std::isfinite(v1)||!(u0<u1&&v0<v1)||u0<s.u.knots.front()||u1>s.u.knots.back()||v0<s.v.knots.front()||v1>s.v.knots.back())return fail<NurbsSource>(e,"Crop requires an ordered rectangle inside a surface domain");
    auto out=s;for(unsigned axis=0;axis<2;++axis){const bool v=axis==1;const double low=v?v0:u0,high=v?v1:u1;auto knots=v?out.v.knots:out.u.knots;if(high<knots.back()){auto p=split_nurbs(out,v,high,e);if(!p)return {};out=std::move(p->first);}if(low>knots.front()){auto p=split_nurbs(out,v,low,e);if(!p)return {};out=std::move(p->second);}}
    return out;
}
std::optional<NurbsSource> fillet_nurbs_corner(const NurbsSource& s,double radius,std::string* e){
    if(!validate_nurbs(s,e))return {};if(!s.is_curve()||s.columns!=3||s.u.degree!=1||!tolerance_ok(radius))return fail<NurbsSource>(e,"Corner fillet requires a three-control degree-one curve and positive radius");
    auto b=point(s.controls[1]),a=sub(point(s.controls[0]),b),c=sub(point(s.controls[2]),b);const double la=norm(a),lc=norm(c);if(la<=1e-12||lc<=1e-12)return fail<NurbsSource>(e,"Corner has a degenerate leg");a=mul(a,1/la);c=mul(c,1/lc);const double theta=std::acos(std::clamp(dot(a,c),-1.0,1.0));if(theta<=1e-6||theta>=3.141592653589793-1e-6)return fail<NurbsSource>(e,"Corner is straight or reversing");
    const double distance=radius/std::tan(theta/2);if(distance>=(std::min)(la,lc))return fail<NurbsSource>(e,"Fillet radius consumes an entire leg; reduce radius");auto x=add(b,mul(a,distance)),y=add(b,mul(c,distance));NurbsSource out;out.columns=3;out.u=uniform_axis(3,2);out.controls={{x[0],x[1],x[2],1},{b[0],b[1],b[2],std::sin(theta/2)},{y[0],y[1],y[2],1}};if(!validate_nurbs(out,e))return {};return out;
}
std::optional<CookedPolygonAsset> tessellate_nurbs_adaptive(const NurbsSource& s,std::uint64_t id,double tolerance,double angle,std::string* e){
    if(!validate_nurbs(s,e))return {};if(s.is_curve()||!tolerance_ok(tolerance)||!std::isfinite(angle)||angle<=0||angle>3.141592653589793)return fail<CookedPolygonAsset>(e,"Adaptive surface bake needs positive chord tolerance and normal angle in (0,pi]");
    std::vector<double> us,vs;for(double t:s.u.knots)if(us.empty()||t!=us.back())us.push_back(t);for(double t:s.v.knots)if(vs.empty()||t!=vs.back())vs.push_back(t);
    double weightScale=0;for(auto p:s.controls)weightScale=(std::max)(weightScale,p.weight);
    unsigned evaluations=0;
    auto sample=[&](double u,double v)->std::optional<NurbsSample>{if(++evaluations>250000)return {};return detail::evaluate_validated_nurbs(s,u,v,weightScale);};
    auto normal=[](const NurbsSample& p){auto n=cross(p.du,p.dv);const double l=norm(n);return l>1e-14?mul(n,1/l):V{};};
    bool finished=false;
    for(unsigned round=0;round<11;++round){
        if(std::size_t(us.size()-1)*(vs.size()-1)>16384)return fail<CookedPolygonAsset>(e,"Adaptive bake exceeds 65536 cell-corner vertices");
        std::vector<double> addU,addV;
        for(std::size_t j=0;j+1<vs.size();++j)for(std::size_t i=0;i+1<us.size();++i){
            const double a=us[i],b=us[i+1],c=vs[j],d=vs[j+1],u=std::midpoint(a,b),v=std::midpoint(c,d);
            if(!(a<u&&u<b&&c<v&&v<d))return fail<CookedPolygonAsset>(e,"Adaptive parameter subdivision lost precision");
            std::array<std::array<double,2>,9> uv{{{a,c},{b,c},{b,d},{a,d},{u,c},{b,v},{u,d},{a,v},{u,v}}};std::array<NurbsSample,9> p;
            for(unsigned k=0;k<9;++k){auto q=sample(uv[k][0],uv[k][1]);if(!q)return fail<CookedPolygonAsset>(e,"Adaptive bake evaluation failed or exceeded work budget");p[k]=*q;}
            double error=norm(sub(p[8].position,mul(add(p[0].position,p[2].position),.5)));
            for(unsigned k=0;k<4;++k)error=(std::max)(error,norm(sub(p[4+k].position,mul(add(p[k].position,p[(k+1)%4].position),.5))));
            auto centreNormal=normal(p[8]);if(norm(centreNormal)<.5)return fail<CookedPolygonAsset>(e,"Adaptive bake encountered a singular surface");bool refine=error>tolerance;
            for(unsigned k=0;k<4;++k){auto q=sample(std::clamp(uv[k][0],std::nextafter(a,b),std::nextafter(b,a)),std::clamp(uv[k][1],std::nextafter(c,d),std::nextafter(d,c)));if(!q)return fail<CookedPolygonAsset>(e,"Adaptive bake exceeded derivative work budget");auto n=normal(*q);if(norm(n)<.5)return fail<CookedPolygonAsset>(e,"Adaptive bake encountered a singular corner");refine|=dot(n,centreNormal)<std::cos(angle);}
            if(refine){addU.push_back(u);addV.push_back(v);}
        }
        if(addU.empty()){finished=true;break;}
        if(round==10)break;
        us.insert(us.end(),addU.begin(),addU.end());vs.insert(vs.end(),addV.begin(),addV.end());std::sort(us.begin(),us.end());std::sort(vs.begin(),vs.end());us.erase(std::unique(us.begin(),us.end()),us.end());vs.erase(std::unique(vs.begin(),vs.end()),vs.end());
    }
    if(!finished)return fail<CookedPolygonAsset>(e,"Adaptive bake did not meet sampled tolerances within subdivision limits");
    CookedPolygonAsset mesh;mesh.objectId=id;VoxelMaterialDefinition material;material.name="Adaptive NURBS";mesh.materials.push_back(material);mesh.materialBindings.resize(1);mesh.materialBindings[0].doubleSided=true;
    for(std::size_t j=0;j+1<vs.size();++j)for(std::size_t i=0;i+1<us.size();++i){
        const double a=us[i],b=us[i+1],c=vs[j],d=vs[j+1];const unsigned first=unsigned(mesh.vertices.size());
        for(auto uv:std::array<std::array<double,2>,4>{{{a,c},{b,c},{b,d},{a,d}}}){
            auto p=sample(uv[0],uv[1]),side=sample(std::clamp(uv[0],std::nextafter(a,b),std::nextafter(b,a)),std::clamp(uv[1],std::nextafter(c,d),std::nextafter(d,c)));if(!p||!side)return fail<CookedPolygonAsset>(e,"Adaptive vertex evaluation exceeded budget");auto n=normal(*side);const double speed=norm(side->du);if(norm(n)<.5||speed<=1e-14)return fail<CookedPolygonAsset>(e,"Adaptive vertex is singular");auto t=mul(side->du,1/speed);
            PolygonVertex v;v.position={float(p->position[0]),float(p->position[1]),float(p->position[2])};v.normal={float(n[0]),float(n[1]),float(n[2])};v.tangent={float(t[0]),float(t[1]),float(t[2]),1};v.texcoord={float(std::lerp(s.textureU[0],s.textureU[1],(uv[0]-us.front())/(us.back()-us.front()))),float(std::lerp(s.textureV[0],s.textureV[1],(uv[1]-vs.front())/(vs.back()-vs.front())))};v.texcoord1=v.texcoord;mesh.vertices.push_back(v);
        }
        mesh.indices.insert(mesh.indices.end(),{first,first+1,first+2,first,first+2,first+3});
    }
    for(std::size_t i=0;i<mesh.indices.size();i+=3){auto a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;if(norm(cross({double(b.x)-a.x,double(b.y)-a.y,double(b.z)-a.z},{double(c.x)-a.x,double(c.y)-a.y,double(c.z)-a.z}))<=0)return fail<CookedPolygonAsset>(e,"Adaptive bake collapsed at float precision");}
    mesh.submeshes.push_back({"Adaptive NURBS",0,unsigned(mesh.indices.size()),0});mesh.bounds.minimum=mesh.bounds.maximum=mesh.vertices.front().position;
    for(auto& v:mesh.vertices){auto p=v.position;mesh.bounds.minimum={(std::min)(mesh.bounds.minimum.x,p.x),(std::min)(mesh.bounds.minimum.y,p.y),(std::min)(mesh.bounds.minimum.z,p.z)};mesh.bounds.maximum={(std::max)(mesh.bounds.maximum.x,p.x),(std::max)(mesh.bounds.maximum.y,p.y),(std::max)(mesh.bounds.maximum.z,p.z)};}
    mesh.contentHash=polygon_asset_content_hash(mesh);auto valid=validate_polygon_asset(mesh);if(!valid){if(e)*e=valid.message;return {};}return mesh;
}
}
