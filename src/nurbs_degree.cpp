#include "dve/nurbs_topology.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
namespace dve {
std::optional<NurbsSource> elevate_nurbs_degree(const NurbsSource& source,bool v,std::string* error){
    auto fail=[&](const char* text)->std::optional<NurbsSource>{if(error)*error=text;return {};};
    if(!validate_nurbs(source,error))return {};
    if(v&&source.is_curve())return fail("A NURBS curve has no V degree");
    const auto& axis=v?source.v:source.u;const unsigned p=axis.degree,q=p+1,n=v?source.rows:source.columns;
    if(p>=7)return fail("NURBS degree is already at the supported maximum of 7");
    std::vector<double> boundaries{axis.knots[p]};
    for(unsigned i=p;i<n;++i)if(axis.knots[i+1]>axis.knots[i])boundaries.push_back(axis.knots[i+1]);
    const unsigned spans=unsigned(boundaries.size()-1),count=spans*q+1,strips=v?source.columns:source.rows;
    if(std::uint64_t(count)*strips>4096)return fail("Degree elevation exceeds the 4096-control budget");
    auto bezier=source;
    // Separate homogeneous Bezier spans without changing their parameter maps.
    for(std::size_t i=1;i+1<boundaries.size();++i){
        const auto t=boundaries[i];const auto& knots=v?bezier.v.knots:bezier.u.knots;
        unsigned multiplicity=unsigned(std::count(knots.begin(),knots.end(),t));
        for(;multiplicity<p;++multiplicity){auto next=insert_nurbs_knot(bezier,v,t,error);if(!next)return {};bezier=std::move(*next);}
    }
    auto out=source;if(v)out.rows=count;else out.columns=count;
    auto& elevated=v?out.v:out.u;elevated.degree=q;elevated.knots.assign(q+1,boundaries.front());
    for(std::size_t i=1;i+1<boundaries.size();++i)elevated.knots.insert(elevated.knots.end(),q,boundaries[i]);
    elevated.knots.insert(elevated.knots.end(),q+1,boundaries.back());
    out.controls.resize(std::size_t(out.rows)*out.columns);
    for(unsigned strip=0;strip<strips;++strip){
        auto get=[&](unsigned i){return bezier.controls[v?i*bezier.columns+strip:strip*bezier.columns+i];};
        auto put=[&](unsigned i,NurbsPoint point){out.controls[v?i*out.columns+strip:strip*out.columns+i]=point;};
        for(unsigned span=0;span<spans;++span)for(unsigned k=0;k<=q;++k){
            if(span&&k==0)continue;
            if(k==0){put(span*q,get(span*p));continue;}
            if(k==q){put(span*q+k,get(span*p+p));continue;}
            const auto a=get(span*p+k-1),b=get(span*p+k);const double alpha=double(k)/q,scale=(std::max)(a.weight,b.weight);
            const double wa=alpha*(a.weight/scale),wb=(1-alpha)*(b.weight/scale),sum=wa+wb;
            if(!(sum>0)||!std::isfinite(sum))return fail("Ill-conditioned homogeneous degree elevation");
            put(span*q+k,{(wa*a.x+wb*b.x)/sum,(wa*a.y+wb*b.y)/sum,(wa*a.z+wb*b.z)/sum,std::lerp(b.weight,a.weight,alpha)});
        }
    }
    if(!validate_nurbs(out,error))return {};
    return out;
}
}
