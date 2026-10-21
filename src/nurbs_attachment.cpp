#include "dve/nurbs_attachment.hpp"
#include "dve/nurbs_modeling.hpp"
#include "dve/nurbs_topology.hpp"
#include <algorithm>
#include <cmath>
#include <map>
namespace dve {
namespace {
bool valid_edge(NurbsBoundary edge){return edge==NurbsBoundary::UMin||edge==NurbsBoundary::UMax||edge==NurbsBoundary::VMin||edge==NurbsBoundary::VMax;}
bool along_v(NurbsBoundary edge){return edge==NurbsBoundary::VMin||edge==NurbsBoundary::VMax;}
bool minimum(NurbsBoundary edge){return edge==NurbsBoundary::UMin||edge==NurbsBoundary::VMin;}
bool normalize(NurbsSource& source,std::string* error){
    for(auto* axis:{&source.u,&source.v}){
        const auto old=axis->knots;const double lo=old.front(),span=old.back()-lo;
        for(std::size_t i=0;i<old.size();++i){
            axis->knots[i]=(old[i]-lo)/span;
            if(!std::isfinite(axis->knots[i])||(i&&old[i]>old[i-1]&&!(axis->knots[i]>axis->knots[i-1]))){
                if(error)*error="Parameter spans lose precision when normalized; rescale the source domain";return false;
            }
        }
    }
    return validate_nurbs(source,error);
}
std::optional<NurbsSource> orient(NurbsSource source,NurbsBoundary edge,bool reference,bool reverse,std::string* error){
    if(along_v(edge)){auto next=transpose_nurbs(source,error);if(!next)return {};source=std::move(*next);}
    if(minimum(edge)==reference){auto next=reverse_nurbs_axis(source,false,error);if(!next)return {};source=std::move(*next);}
    if(reverse){auto next=reverse_nurbs_axis(source,true,error);if(!next)return {};source=std::move(*next);}
    return source;
}
std::optional<NurbsSource> restore(NurbsSource source,const NurbsSource& original,NurbsBoundary edge,bool reference,bool reverse,std::string* error){
    if(reverse){auto next=reverse_nurbs_axis(source,true,error);if(!next)return {};source=std::move(*next);}
    if(minimum(edge)==reference){auto next=reverse_nurbs_axis(source,false,error);if(!next)return {};source=std::move(*next);}
    if(along_v(edge)){auto next=transpose_nurbs(source,error);if(!next)return {};source=std::move(*next);}
    auto map=[](NurbsAxis& axis,const NurbsAxis& input){for(auto& knot:axis.knots)knot=std::lerp(input.knots.front(),input.knots.back(),knot);};
    map(source.u,original.u);map(source.v,original.v);
    source.textureU=original.textureU;source.textureV=original.textureV;
    if(!validate_nurbs(source,error))return {};
    return source;
}
}
std::optional<NurbsAttachment> attach_nurbs_boundaries(const NurbsSource& reference,const NurbsSource& moving,NurbsAttachmentOptions options,std::string* error){
    std::string reason;
    auto fail=[&](const std::string& message)->std::optional<NurbsAttachment>{if(error)*error="Boundary attachment: "+message;return {};};
    if(!valid_edge(options.referenceBoundary)||!valid_edge(options.movingBoundary))return fail("Choose a U-min, U-max, V-min or V-max boundary");
    if(!validate_nurbs(reference,&reason)||!validate_nurbs(moving,&reason))return fail(reason);
    if(reference.is_curve()||moving.is_curve())return fail("Select two untrimmed NURBS surfaces");
    auto a=reference,b=moving;
    if(!normalize(a,&reason)||!normalize(b,&reason))return fail(reason);
    auto ar=orient(std::move(a),options.referenceBoundary,true,false,&reason),br=orient(std::move(b),options.movingBoundary,false,options.reverseBoundary,&reason);
    if(!ar||!br)return fail(reason);
    a=std::move(*ar);b=std::move(*br);
    const auto degree=(std::max)(a.v.degree,b.v.degree);
    for(auto* source:{&a,&b})while(source->v.degree<degree){
        auto next=elevate_nurbs_degree(*source,true,&reason);if(!next)return fail(reason);*source=std::move(*next);
    }
    // Exact multiplicity union. Nearby distinct knots remain distinct.
    std::map<double,unsigned> knots;
    for(const auto* source:{&a,&b}){
        std::map<double,unsigned> counts;for(double knot:source->v.knots)++counts[knot];
        for(auto [knot,count]:counts)knots[knot]=(std::max)(knots[knot],count);
    }
    for(auto* source:{&a,&b})for(auto [knot,count]:knots){
        if(knot==0||knot==1)continue;
        auto existing=unsigned(std::count(source->v.knots.begin(),source->v.knots.end(),knot));
        while(existing++<count){auto next=insert_nurbs_knot(*source,true,knot,&reason);if(!next)return fail(reason);*source=std::move(*next);}
    }
    // A linear two-column patch needs a free tangent row to keep its far edge.
    if(options.c1&&b.columns<3){auto next=elevate_nurbs_degree(b,false,&reason);if(!next)return fail(reason);b=std::move(*next);}
    if(a.v!=b.v||a.rows!=b.rows)return fail("Boundary knot compatibility could not be represented exactly");
    auto matched=attach_nurbs_u(a,b,options.c1,&reason);if(!matched)return fail(reason);
    auto prepared=restore(std::move(a),reference,options.referenceBoundary,true,false,&reason);
    auto result=restore(std::move(*matched),moving,options.movingBoundary,false,options.reverseBoundary,&reason);
    if(!prepared||!result)return fail(reason);
    return NurbsAttachment{std::move(*prepared),std::move(*result)};
}
}
