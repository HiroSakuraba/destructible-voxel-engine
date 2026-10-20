#include "dve/nurbs_topology.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
bool refuse(std::string* e,const char* message){if(e)*e=message;return false;}
bool axis_ready(const NurbsSource& s,bool v,std::string* e){
    return validate_nurbs(s,e)&&(!(v&&s.is_curve())||refuse(e,"A NURBS curve has no V axis"));
}
std::optional<NurbsSource> refine_boundary(const NurbsSource& s,bool v,double t,std::string* e){
    auto out=s;const auto degree=v?s.v.degree:s.u.degree;
    const auto& knots=v?s.v.knots:s.u.knots;
    auto multiplicity=unsigned(std::count(knots.begin(),knots.end(),t));
    for(;multiplicity<degree;++multiplicity){auto next=insert_nurbs_knot(out,v,t,e);if(!next)return {};out=std::move(*next);}
    return out;
}
}
std::optional<NurbsSource> reverse_nurbs_axis(const NurbsSource& s,bool v,std::string* e){
    if(!axis_ready(s,v,e))return {};
    auto out=s;auto& axis=v?out.v:out.u;const auto& old=v?s.v:s.u;
    const double lo=old.knots.front(),hi=old.knots.back();
    for(std::size_t i=0;i<axis.knots.size();++i)axis.knots[i]=lo+(hi-old.knots[axis.knots.size()-1-i]);
    for(unsigned j=0;j<s.rows;++j)for(unsigned i=0;i<s.columns;++i)
        out.controls[j*s.columns+i]=s.controls[(v?s.rows-1-j:j)*s.columns+(v?i:s.columns-1-i)];
    if(!validate_nurbs(out,e))return {};
    return out;
}
std::optional<NurbsSource> transpose_nurbs(const NurbsSource& s,std::string* e){
    if(!axis_ready(s,true,e))return {};
    auto out=s;std::swap(out.columns,out.rows);std::swap(out.u,out.v);std::swap(out.textureU,out.textureV);
    for(unsigned j=0;j<s.rows;++j)for(unsigned i=0;i<s.columns;++i)out.controls[i*out.columns+j]=s.controls[j*s.columns+i];
    if(!validate_nurbs(out,e))return {};
    return out;
}
std::optional<std::pair<NurbsSource,NurbsSource>> split_nurbs(const NurbsSource& s,bool v,double t,std::string* e){
    if(!axis_ready(s,v,e))return {};
    const auto& axis=v?s.v:s.u;const auto n=v?s.rows:s.columns;
    if(!std::isfinite(t)||t<=axis.knots[axis.degree]||t>=axis.knots[n]){refuse(e,"Split parameter must be strictly inside the axis domain");return {};}
    auto refined=refine_boundary(s,v,t,e);if(!refined)return {};
    const auto& knots=v?refined->v.knots:refined->u.knots;
    const unsigned last=unsigned(std::upper_bound(knots.begin(),knots.end(),t)-knots.begin()-1);
    const unsigned seam=last-axis.degree,count=v?refined->rows:refined->columns;
    auto left=*refined,right=*refined;
    auto& leftAxis=v?left.v:left.u;auto& rightAxis=v?right.v:right.u;
    leftAxis.knots.assign(knots.begin(),knots.begin()+last+1);leftAxis.knots.push_back(t);
    rightAxis.knots.assign(1,t);rightAxis.knots.insert(rightAxis.knots.end(),knots.begin()+seam+1,knots.end());
    if(v){left.rows=seam+1;right.rows=count-seam;}else{left.columns=seam+1;right.columns=count-seam;}
    auto copy=[&](NurbsSource& out,unsigned offset){
        out.controls.clear();out.controls.reserve(std::size_t(out.rows)*out.columns);
        for(unsigned j=0;j<out.rows;++j)for(unsigned i=0;i<out.columns;++i)
            out.controls.push_back(refined->controls[(j+(v?offset:0))*refined->columns+i+(v?0:offset)]);
    };
    copy(left,0);copy(right,seam);
    const auto range=v?s.textureV:s.textureU;
    const double fraction=(t-axis.knots[axis.degree])/(axis.knots[n]-axis.knots[axis.degree]);
    const double texture=std::lerp(range[0],range[1],fraction);
    (v?left.textureV:left.textureU)=std::array<double,2>{range[0],texture};
    (v?right.textureV:right.textureU)=std::array<double,2>{texture,range[1]};
    if(!validate_nurbs(left,e)||!validate_nurbs(right,e))return {};
    return std::pair{std::move(left),std::move(right)};
}
std::optional<NurbsSource> extract_nurbs_isocurve(const NurbsSource& s,bool v,double t,std::string* e){
    if(!validate_nurbs(s,e))return {};
    if(s.is_curve()){refuse(e,"Isocurve extraction requires a NURBS surface");return {};}
    const auto& axis=v?s.v:s.u;const auto n=v?s.rows:s.columns;
    const double lo=axis.knots[axis.degree],hi=axis.knots[n];
    if(!std::isfinite(t)||t<lo||t>hi){refuse(e,"Isocurve parameter is outside the axis domain");return {};}
    auto refined=(t==lo||t==hi)?std::optional<NurbsSource>(s):refine_boundary(s,v,t,e);if(!refined)return {};
    const auto& knots=v?refined->v.knots:refined->u.knots;
    unsigned index=0;
    if(t==hi)index=(v?refined->rows:refined->columns)-1;
    else if(t!=lo)index=unsigned(std::upper_bound(knots.begin(),knots.end(),t)-knots.begin()-1)-axis.degree;
    NurbsSource out;out.columns=v?refined->columns:refined->rows;out.u=v?refined->u:refined->v;out.samplesPerSpan=s.samplesPerSpan;out.creaseNormals=s.creaseNormals;out.textureU=v?s.textureU:s.textureV;
    for(unsigned i=0;i<out.columns;++i)out.controls.push_back(refined->controls[v?index*refined->columns+i:i*refined->columns+index]);
    if(!validate_nurbs(out,e))return {};
    return out;
}
}
