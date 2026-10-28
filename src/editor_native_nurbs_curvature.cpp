#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/nurbs_curvature.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

// Curvature comb overlay and the continuity report for native NURBS sources.
namespace dve::editor {

void NativeEditorController::draw_nurbs_curvature(const IEditorCanvas& canvas,UiRect viewport) const{
    if(!showNurbsCurvature_||playSession_.active())return;
    constexpr EditorColor tooth=0xff3fa7f2,envelope=0xff2b7fd6;
    unsigned drawn=0;
    for(auto id:workspace_.selected_objects()){
        if(++drawn>16)break;
        const auto* o=workspace_.document().find_object(id);
        if(!o||!o->flags.visible||is_isolated_out(id)||!has_nurbs_source(*o))continue;
        auto source=world_editor_nurbs_source(workspace_.document(),*o);
        if(!source||!source->is_curve())continue;
        auto comb=nurbs_curvature_comb(*source,16);
        if(!comb||comb->samples.empty()||!(comb->maximumCurvature>0))continue;
        // Longest tooth: a quarter of the curve's extent, so combs stay readable at any scale.
        double lo[3]{std::numeric_limits<double>::max(),std::numeric_limits<double>::max(),std::numeric_limits<double>::max()},hi[3]{-lo[0],-lo[1],-lo[2]};
        for(const auto& p:comb->samples)for(unsigned k=0;k<3;++k){lo[k]=std::min(lo[k],p.position[k]);hi[k]=std::max(hi[k],p.position[k]);}
        const double extent=std::hypot(hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2]);
        const double scale=.25*extent/comb->maximumCurvature;
        std::optional<ScreenPoint> previous;
        for(const auto& p:comb->samples){
            // Teeth point away from the centre of curvature (the usual comb convention).
            const Float3 base{float(p.position[0]),float(p.position[1]),float(p.position[2])};
            const Float3 tip{float(p.position[0]-p.curvatureVector[0]*scale),float(p.position[1]-p.curvatureVector[1]*scale),float(p.position[2]-p.curvatureVector[2]*scale)};
            const auto a=project_world_to_screen(camera_,viewport,base),b=project_world_to_screen(camera_,viewport,tip);
            if(a.visible&&b.visible)canvas.line(int(a.x),int(a.y),int(b.x),int(b.y),tooth,1);
            if(b.visible&&previous&&previous->visible)canvas.line(int(previous->x),int(previous->y),int(b.x),int(b.y),envelope,1);
            previous=b;
        }
    }
}

bool NativeEditorController::report_nurbs_continuity(){
    auto& document=workspace_.document();
    const auto active=workspace_.selected_object();
    const auto count=workspace_.selection_count();
    if(!active||count<1||count>2){set_status("Select one NURBS curve, or two curves or two surfaces (the active one is A)",true);return false;}
    std::optional<NurbsSource> a,b;std::string error;
    for(auto id:workspace_.selected_objects()){
        const auto* o=document.find_object(id);
        if(!o||!has_nurbs_source(*o)||is_isolated_out(id)){set_status("Select native NURBS sources inside the isolation",true);return false;}
        auto s=world_editor_nurbs_source(document,*o,&error);if(!s){set_status(error,true);return false;}
        (id==*active?a:b)=std::move(*s);
    }
    std::string description;
    if(count==1){
        if(!a->is_curve()){set_status("For a surface, select it and the patch it joins; one curve reports its own knots",true);return false;}
        auto report=nurbs_curve_internal_continuity(*a,{},&error);if(!report){set_status(error,true);return false;}
        description=report->places.empty()?"The curve has no interior knots: it is one polynomial piece, so it is curvature continuous throughout":report->description;
    }else if(a->is_curve()&&b->is_curve()){
        auto report=nurbs_curve_join_continuity(*a,*b,{},&error);if(!report){set_status(error,true);return false;}
        description=report->description;
    }else if(!a->is_curve()&&!b->is_curve()){
        auto report=nurbs_surface_join_continuity(*a,*b,17,{},&error);if(!report){set_status(error,true);return false;}
        description=report->description;
    }else{set_status("Continuity compares two curves or two surfaces, not one of each",true);return false;}
    set_status(description+" (world units, sampled)",false,20.0F);
    return true;
}

bool NativeEditorController::bridge_nurbs_curves(){
    auto& document=workspace_.document();
    if(workspace_.selection_count()!=2){set_status("Select two NURBS curves to bridge; the nearest ends are joined",true);return false;}
    std::vector<NurbsSource> curves;std::string error;
    for(auto id:workspace_.selected_objects()){
        const auto* o=document.find_object(id);
        if(!o||!has_nurbs_source(*o)||!o->flags.visible||is_isolated_out(id)){set_status("Select two visible native NURBS curves",true);return false;}
        auto s=world_editor_nurbs_source(document,*o,&error);if(!s){set_status(error,true);return false;}
        if(!s->is_curve()){set_status("A G2 bridge joins two curves, not surfaces",true);return false;}
        curves.push_back(std::move(*s));
    }
    auto bridge=bridge_nurbs_curves_g2(curves[0],curves[1],&error,true);
    if(!bridge){set_status(error,true);return false;}
    // Built in world space, so the new object has the identity pose and unit scale.
    if(!create_nurbs_source(bridge->source,bridge->tangentOnly?"NURBS G1 Bridge":"NURBS G2 Bridge",std::pair{RigidTransform{},Float3{1,1,1}}))return false;
    auto first=nurbs_curve_join_continuity(curves[0],bridge->source),second=nurbs_curve_join_continuity(curves[1],bridge->source);
    std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(4);
    if(bridge->tangentOnly){
        text<<"G1 bridge added (tangent only; curvature jumps at the ends). "<<bridge->fallbackReason<<". Ends: "
            <<(first?nurbs_continuity_name(first->level):"?")<<" / "<<(second?nurbs_continuity_name(second->level):"?")<<"; one undo step";
        set_status(text.str(),false,20.0F);
        return true;
    }
    text<<"G2 bridge added (cubic; handles "<<bridge->handleA<<" and "<<bridge->handleB<<" world units; "<<bridge->solutions
        <<(bridge->solutions==1?" solution":" solutions, kept the most stable")<<"). Ends: "
        <<(first?nurbs_continuity_name(first->level):"?")<<" / "<<(second?nurbs_continuity_name(second->level):"?")<<"; one undo step";
    set_status(text.str(),false,15.0F);
    return true;
}

} // namespace dve::editor
