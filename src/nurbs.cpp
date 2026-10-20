#include "dve/nurbs.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <set>

namespace dve {
namespace {
bool fail(std::string* e,const char* s){if(e)*e=s;return false;}
constexpr unsigned maxControls=4096, maxDegree=7, maxSamples=64;
bool axis_valid(const NurbsAxis& a,unsigned n,bool curveV,std::string* e) {
    if(curveV)return a.degree==0&&a.knots==std::vector<double>{0,1};
    if(a.degree<1||a.degree>maxDegree||n<=a.degree||a.knots.size()!=n+a.degree+1)
        return fail(e,"Invalid NURBS degree, control count or knot count");
    for(std::size_t i=0;i<a.knots.size();++i)
        if(!std::isfinite(a.knots[i])||(i&&a.knots[i]<a.knots[i-1]))return fail(e,"Knots must be finite and nondecreasing");
    const double lo=a.knots[a.degree],hi=a.knots[n];
    if(!(hi>lo)||!std::isfinite(hi-lo))return fail(e,"NURBS domain is empty or unrepresentable");
    for(unsigned i=0;i<=a.degree;++i)
        if(a.knots[i]!=lo||a.knots[n+i]!=hi)return fail(e,"First release requires clamped NURBS axes");
    unsigned run=1;
    for(unsigned i=a.degree+1;i<n;++i) {
        run=a.knots[i]==a.knots[i-1]?run+1:1;
        if(!(a.knots[i]>lo&&a.knots[i]<hi)||run>a.degree)return fail(e,"Invalid interior knot multiplicity");
    }
    return true;
}
struct Basis { unsigned first{}; std::array<double,8> b{},d{}; };
// Local Cox-de Boor basis and derivative via lower-degree basis. At interior
// knots choose the right-hand span; the upper endpoint chooses the left side.
std::array<double,8> basis_degree(const NurbsAxis& a,unsigned span,unsigned p,double t) {
    std::array<double,8> b{},left{},right{};b[0]=1;
    for(unsigned j=1;j<=p;++j) {
        left[j]=t-a.knots[span+1-j];right[j]=a.knots[span+j]-t;double saved=0;
        for(unsigned r=0;r<j;++r) {
            const double den=right[r+1]+left[j-r];
            const double term=den>0?b[r]/den:0;
            b[r]=saved+right[r+1]*term;saved=left[j-r]*term;
        }
        b[j]=saved;
    }
    return b;
}
Basis basis(const NurbsAxis& a,unsigned n,double t,bool fromLeft=false) {
    if(a.degree==0){Basis b;b.b[0]=1;return b;}
    unsigned span=n-1;
    if(t<a.knots[n]){
        const auto begin=a.knots.begin()+a.degree,end=a.knots.begin()+n+1;
        span=unsigned((fromLeft&&t>a.knots[a.degree]?std::lower_bound(begin,end,t):std::upper_bound(begin,end,t))-a.knots.begin()-1);
    }
    Basis out;out.first=span-a.degree;out.b=basis_degree(a,span,a.degree,t);
    const auto lower=basis_degree(a,span,a.degree-1,t);
    for(unsigned k=0;k<=a.degree;++k) {
        unsigned i=out.first+k;
        const double l=a.knots[i+a.degree]-a.knots[i],r=a.knots[i+a.degree+1]-a.knots[i+1];
        out.d[k]=a.degree*((k&&l>0?lower[k-1]/l:0)-(k<a.degree&&r>0?lower[k]/r:0));
    }
    return out;
}
std::optional<NurbsSample> eval(const NurbsSource& s,double u,double v,std::string* e,double scale=0,bool leftU=false,bool leftV=false) {
    if(!std::isfinite(u)||!std::isfinite(v)||u<s.u.knots[s.u.degree]||u>s.u.knots[s.columns]||
       (!s.is_curve()&&(v<s.v.knots[s.v.degree]||v>s.v.knots[s.rows]))) {
        fail(e,"Parameter outside NURBS domain");return {};
    }
    auto a=basis(s.u,s.columns,u,leftU),b=basis(s.v,s.rows,v,leftV);
    std::array<double,4> h{},hu{},hv{};
    // Common weight scale preserves rational geometry and avoids denominator overflow.
    if(scale==0)for(const auto& p:s.controls)scale=(std::max)(scale,p.weight);
    for(unsigned j=0;j<=s.v.degree;++j)for(unsigned i=0;i<=s.u.degree;++i){
        const auto& p=s.controls[(b.first+j)*s.columns+a.first+i];
        const double w=p.weight/scale;
        const std::array<double,4> q{p.x*w,p.y*w,p.z*w,w};
        for(unsigned c=0;c<4;++c){h[c]+=q[c]*a.b[i]*b.b[j];hu[c]+=q[c]*a.d[i]*b.b[j];hv[c]+=q[c]*a.b[i]*b.d[j];}
    }
    if(!(h[3]>1e-14)||!std::isfinite(h[3])){fail(e,"Ill-conditioned NURBS denominator");return {};}
    NurbsSample out;
    for(unsigned c=0;c<3;++c) {
        out.position[c]=h[c]/h[3];out.du[c]=(hu[c]-out.position[c]*hu[3])/h[3];out.dv[c]=(hv[c]-out.position[c]*hv[3])/h[3];
        if(!std::isfinite(out.position[c])||!std::isfinite(out.du[c])||!std::isfinite(out.dv[c])){
            fail(e,"NURBS evaluation overflow");return {};
        }
    }
    return out;
}
std::size_t sample_count(const NurbsAxis& a,unsigned n,unsigned count,bool creases=false){
    if(a.degree==0)return 1;
    std::size_t spans=0,seams=0;
    for(unsigned i=a.degree;i<n;++i)if(a.knots[i+1]>a.knots[i]){
        if(creases&&spans){
            const auto t=a.knots[i];
            const auto multiplicity=std::upper_bound(a.knots.begin(),a.knots.end(),t)-std::lower_bound(a.knots.begin(),a.knots.end(),t);
            if(multiplicity>=a.degree)++seams;
        }
        ++spans;
    }
    return spans*count+seams+1;
}
struct DisplaySample { double t{}; bool fromLeft{}; };
std::vector<DisplaySample> display_samples(const NurbsAxis& a,unsigned n,unsigned count,bool creases){
    if(a.degree==0)return {{0,false}};
    std::vector<DisplaySample> out;
    for(unsigned i=a.degree;i<n;++i)if(a.knots[i+1]>a.knots[i]){
        const double t=a.knots[i];
        const auto multiplicity=std::upper_bound(a.knots.begin(),a.knots.end(),t)-std::lower_bound(a.knots.begin(),a.knots.end(),t);
        if(creases&&!out.empty()&&multiplicity>=a.degree)out.push_back({t,true});
        for(unsigned j=0;j<count;++j)out.push_back({std::lerp(t,a.knots[i+1],double(j)/count),false});
    }
    out.push_back({a.knots[n],true});return out;
}
Float3 vec(const std::array<double,3>& v){return {float(v[0]),float(v[1]),float(v[2])};}
Float3 cross3(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Float3 unit(Float3 a){const auto l=length(a);return l>1e-12F?multiply(a,1/l):Float3{};}
}
bool validate_nurbs(const NurbsSource& s,std::string* e) {
    if(s.columns>maxControls||s.rows<1||s.rows>maxControls||std::uint64_t(s.columns)*s.rows>maxControls||
       s.controls.size()!=std::uint64_t(s.columns)*s.rows||s.samplesPerSpan<1||s.samplesPerSpan>maxSamples)
        return fail(e,"NURBS control/sample budget exceeded or inconsistent net");
    if(!axis_valid(s.u,s.columns,false,e)||!axis_valid(s.v,s.rows,s.is_curve(),e))return false;
    for(const auto& range:{s.textureU,s.textureV})
        if(!std::isfinite(range[0])||!std::isfinite(range[1])||std::abs(range[0])>1e6||std::abs(range[1])>1e6||!(range[1]>range[0]))
            return fail(e,"NURBS texture ranges must be finite, increasing and bounded to +/-1000000");
    double minW=std::numeric_limits<double>::max(),maxW=0;
    for(const auto& p:s.controls) {
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||std::abs(p.x)>1e8||std::abs(p.y)>1e8||std::abs(p.z)>1e8||
           !std::isfinite(p.weight)||p.weight<=0)return fail(e,"NURBS controls require bounded finite positions and positive finite weights");
        minW=(std::min)(minW,p.weight);maxW=(std::max)(maxW,p.weight);
    }
    if(minW/maxW<1e-12)return fail(e,"NURBS weight ratio is ill-conditioned");
    auto a=sample_count(s.u,s.columns,s.samplesPerSpan),b=sample_count(s.v,s.rows,s.samplesPerSpan);
    if(a>65536||b>65536||a*b>65536)return fail(e,"NURBS tessellation exceeds 65536 sample budget");
    if(s.creaseNormals){
        const auto u=sample_count(s.u,s.columns,s.samplesPerSpan,true),v=sample_count(s.v,s.rows,s.samplesPerSpan,true);
        const auto rows=s.is_curve()?2:v;
        if(u*rows>65536)return fail(e,"Crease-aware NURBS mesh exceeds 65536 vertices");
    }
    return true;
}
std::optional<NurbsSample> evaluate_nurbs(const NurbsSource& s,double u,double v,std::string* e){
    if(!validate_nurbs(s,e))return {};
    return eval(s,u,v,e);
}
NurbsSource make_nurbs_patch(double size) {
    NurbsSource s;s.columns=s.rows=4;s.u.knots={0,0,0,0,1,1,1,1};s.v=s.u;
    for(unsigned j=0;j<4;++j)for(unsigned i=0;i<4;++i)s.controls.push_back({size*(double(i)/3-.5),0,size*(.5-double(j)/3),1});
    return s;
}
NurbsSource make_nurbs_arc(double r,double angle){
    NurbsSource s;s.u.degree=2;
    if(!std::isfinite(r)||r<=0||!std::isfinite(angle)||std::abs(angle)<1e-6||std::abs(angle)>6.283185307179587)return s;
    unsigned segments=unsigned(std::ceil(std::abs(angle)/1.5707963267948966));s.columns=2*segments+1;
    s.u.knots={0,0,0};
    for(unsigned i=0;i<=2*segments;++i) {
        const double t=angle*double(i)/(2*segments),w=i%2?std::cos(angle/(2*segments)):1;
        s.controls.push_back({r*std::cos(t)/w,0,r*std::sin(t)/w,w});
        if(i>0&&i<2*segments&&i%2==0){s.u.knots.push_back(double(i)/(2*segments));s.u.knots.push_back(double(i)/(2*segments));}
    }
    s.u.knots.insert(s.u.knots.end(),{1,1,1});
    if(std::abs(std::abs(angle)-6.283185307179586)<1e-12)s.controls.back()=s.controls.front();
    return s;
}
std::optional<NurbsSource> extrude_nurbs(const NurbsSource& c,std::array<double,3> d,std::string* e){
    if(!validate_nurbs(c,e)||!c.is_curve()){fail(e,"Extrude requires a valid NURBS curve");return {};}
    if(!std::isfinite(d[0])||!std::isfinite(d[1])||!std::isfinite(d[2])||d==std::array<double,3>{}){fail(e,"Extrusion vector must be finite and nonzero");return {};}
    auto s=c;s.rows=2;s.v={1,{0,0,1,1}};
    for(auto p:c.controls){p.x+=d[0];p.y+=d[1];p.z+=d[2];s.controls.push_back(p);}
    if(!validate_nurbs(s,e))return {};
    return s;
}
std::optional<NurbsSource> revolve_nurbs(const NurbsSource& c,double angle,std::string* e){
    if(!validate_nurbs(c,e)||!c.is_curve()){fail(e,"Revolve requires a valid NURBS curve");return {};}
    const auto arc=make_nurbs_arc(1,angle);if(!validate_nurbs(arc,e))return {};
    auto s=c;s.rows=arc.columns;s.v=arc.u;s.controls.clear();
    for(const auto& a:arc.controls)for(auto p:c.controls){
        s.controls.push_back({p.x*a.x-p.z*a.z,p.y,p.x*a.z+p.z*a.x,p.weight*a.weight});
    }
    if(!validate_nurbs(s,e))return {};
    return s;
}
std::optional<NurbsSource> insert_nurbs_knot(const NurbsSource& source,bool alongV,double t,std::string* e){
    if(!validate_nurbs(source,e))return {};
    if(alongV&&source.is_curve()){fail(e,"A curve has no V knot axis");return {};}
    const auto& axis=alongV?source.v:source.u;
    const unsigned count=alongV?source.rows:source.columns,p=axis.degree;
    if(!std::isfinite(t)||!(t>axis.knots[p]&&t<axis.knots[count])){
        fail(e,"Insert knots strictly inside the parameter domain");return {};
    }
    const unsigned multiplicity=unsigned(std::count(axis.knots.begin(),axis.knots.end(),t));
    if(multiplicity>=p){fail(e,"Knot multiplicity already equals the degree");return {};}
    const unsigned span=unsigned(std::upper_bound(axis.knots.begin(),axis.knots.end(),t)-axis.knots.begin()-1);
    auto out=source;
    if(alongV)++out.rows;else ++out.columns;
    if(std::uint64_t(out.rows)*out.columns>4096){fail(e,"Knot insertion exceeds control budget");return {};}
    auto& nextAxis=alongV?out.v:out.u;
    nextAxis.knots.insert(nextAxis.knots.begin()+span+1,t);
    out.controls.resize(std::size_t(out.rows)*out.columns);
    const unsigned strips=alongV?source.columns:source.rows;
    for(unsigned strip=0;strip<strips;++strip){
        auto get=[&](unsigned i){return source.controls[alongV?i*source.columns+strip:strip*source.columns+i];};
        auto put=[&](unsigned i,NurbsPoint point){out.controls[alongV?i*out.columns+strip:strip*out.columns+i]=point;};
        for(unsigned i=0;i<=span-p;++i)put(i,get(i));
        for(unsigned i=span-multiplicity;i<count;++i)put(i+1,get(i));
        for(unsigned i=span-p+1;i<=span-multiplicity;++i){
            const double alpha=(t-axis.knots[i])/(axis.knots[i+p]-axis.knots[i]);
            const auto a=get(i-1),b=get(i);
            const double scale=(std::max)(a.weight,b.weight),wa=(1-alpha)*(a.weight/scale),wb=alpha*(b.weight/scale),sum=wa+wb;
            if(!(sum>0)||!std::isfinite(sum)){fail(e,"Ill-conditioned knot insertion");return {};}
            put(i,{(wa*a.x+wb*b.x)/sum,(wa*a.y+wb*b.y)/sum,(wa*a.z+wb*b.z)/sum,std::lerp(a.weight,b.weight,alpha)});
        }
    }
    if(!validate_nurbs(out,e))return {};
    return out;
}
std::string encode_nurbs(const NurbsSource& s){
    std::string error;if(!validate_nurbs(s,&error))return {};
    std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(17)<<"DVE_NURBS 2 "<<s.columns<<' '<<s.rows<<' '<<s.samplesPerSpan<<' '<<unsigned(s.creaseNormals)<<' '<<s.textureU[0]<<' '<<s.textureU[1]<<' '<<s.textureV[0]<<' '<<s.textureV[1]<<' ';
    for(const auto* a:{&s.u,&s.v}){o<<a->degree<<' '<<a->knots.size()<<' ';for(auto k:a->knots)o<<k<<' ';}
    for(auto p:s.controls)o<<p.x<<' '<<p.y<<' '<<p.z<<' '<<p.weight<<' ';
    return o.str();
}
std::optional<NurbsSource> decode_nurbs(std::string_view text,std::string* e){
    if(text.size()>1024*1024){fail(e,"NURBS source exceeds codec byte budget");return {};}
    std::istringstream in{std::string(text)};in.imbue(std::locale::classic());std::string magic;unsigned version;NurbsSource s;
    if(!(in>>magic>>version>>s.columns>>s.rows>>s.samplesPerSpan)||magic!="DVE_NURBS"||(version!=1&&version!=2)||
       s.columns>maxControls||s.rows>maxControls||s.rows<1||std::uint64_t(s.columns)*s.rows>maxControls){fail(e,"Invalid NURBS source header");return {};}
    if(version==1)s.creaseNormals=false;
    else {
        unsigned creases;
        if(!(in>>creases>>s.textureU[0]>>s.textureU[1]>>s.textureV[0]>>s.textureV[1])||creases>1){fail(e,"Invalid NURBS display metadata");return {};}
        s.creaseNormals=creases!=0;
    }
    for(auto* a:{&s.u,&s.v}){
        std::size_t count;if(!(in>>a->degree>>count)||count>maxControls+maxDegree+1){fail(e,"Invalid NURBS axis header");return {};}
        a->knots.resize(count);for(auto& k:a->knots)if(!(in>>k)){fail(e,"Truncated NURBS knots");return {};}
    }
    s.controls.resize(std::size_t(s.columns)*s.rows);
    for(auto& p:s.controls)if(!(in>>p.x>>p.y>>p.z>>p.weight)){fail(e,"Truncated NURBS controls");return {};}
    in>>std::ws;if(!in.eof()||!validate_nurbs(s,e)){if(e&&e->empty())*e="Trailing NURBS source data";return {};}return s;
}
std::optional<CookedPolygonAsset> tessellate_nurbs(const NurbsSource& s,std::uint64_t id,std::string* e){
    if(!validate_nurbs(s,e))return {};
    auto us=display_samples(s.u,s.columns,s.samplesPerSpan,s.creaseNormals),vs=display_samples(s.v,s.rows,s.samplesPerSpan,s.creaseNormals);
    CookedPolygonAsset m;m.objectId=id;
    VoxelMaterialDefinition material;material.name="NURBS";m.materials.push_back(material);
    m.materialBindings.resize(1);m.materialBindings[0].doubleSided=true;
    double scale=0;for(const auto& p:s.controls)scale=(std::max)(scale,p.weight);
    const unsigned columns=unsigned(us.size()),rows=s.is_curve()?2:unsigned(vs.size());
    for(unsigned j=0;j<rows;++j)for(unsigned i=0;i<columns;++i){
        auto q=eval(s,us[i].t,s.is_curve()?0:vs[j].t,e,scale,us[i].fromLeft,!s.is_curve()&&vs[j].fromLeft);if(!q)return {};
        PolygonVertex v;v.position=vec(q->position);auto tangent=unit(vec(q->du));
        auto n=s.is_curve()?unit(cross3(tangent,std::abs(tangent.y)<.9F?Float3{0,1,0}:Float3{0,0,1})):unit(cross3(vec(q->du),vec(q->dv)));
        if(length(n)<.5F){fail(e,"Singular NURBS sample; move controls away from a collapsed span");return {};}
        if(s.is_curve())v.position=add(v.position,multiply(n,j?.015F:-.015F));
        v.normal=s.is_curve()?unit(cross3(tangent,n)):n;
        v.texcoord={float(std::lerp(s.textureU[0],s.textureU[1],(us[i].t-s.u.knots[s.u.degree])/(s.u.knots[s.columns]-s.u.knots[s.u.degree]))),
            s.is_curve()?float(j):float(std::lerp(s.textureV[0],s.textureV[1],(vs[j].t-s.v.knots[s.v.degree])/(s.v.knots[s.rows]-s.v.knots[s.v.degree])))};v.texcoord1=v.texcoord;
        v.tangent={tangent.x,tangent.y,tangent.z,1};m.vertices.push_back(v);
    }
    for(unsigned j=0;j+1<rows;++j)for(unsigned i=0;i+1<columns;++i){
        if(us[i].t==us[i+1].t||(!s.is_curve()&&vs[j].t==vs[j+1].t))continue;
        auto a=j*columns+i,b=a+1,c=a+columns,d=c+1;m.indices.insert(m.indices.end(),{a,b,d,a,d,c});
    }
    for(std::size_t i=0;i<m.indices.size();i+=3) {
        const auto a=m.vertices[m.indices[i]].position,b=m.vertices[m.indices[i+1]].position,c=m.vertices[m.indices[i+2]].position;
        const auto area=cross3(subtract(b,a),subtract(c,a));
        if(!(length(area)>0)||!std::isfinite(length(area))){fail(e,"NURBS bake collapsed at float precision; adjust controls or object scale");return {};}
    }
    m.submeshes.push_back({"NURBS",0,unsigned(m.indices.size()),0});
    // Asset hash and bounds use the normal production polygon pipeline.
    m.bounds.minimum=m.bounds.maximum=m.vertices.front().position;
    for(const auto& v:m.vertices){auto p=v.position;m.bounds.minimum={std::min(m.bounds.minimum.x,p.x),std::min(m.bounds.minimum.y,p.y),std::min(m.bounds.minimum.z,p.z)};m.bounds.maximum={std::max(m.bounds.maximum.x,p.x),std::max(m.bounds.maximum.y,p.y),std::max(m.bounds.maximum.z,p.z)};}
    m.contentHash=polygon_asset_content_hash(m);
    auto valid=validate_polygon_asset(m);if(!valid){if(e)*e=valid.message;return {};}return m;
}
}

