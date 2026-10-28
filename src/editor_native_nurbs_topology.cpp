#include "dve/editor_native.hpp"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include "dve/nurbs_queries.hpp"
#include "dve/nurbs_modeling.hpp"
#include "dve/editor_nurbs_seam.hpp"
#include "dve/editor_nurbs_surface.hpp"
#include <algorithm>
namespace dve::editor {
bool NativeEditorController::dispatch_nurbs_topology(std::string_view action){
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active()||longJob_||gizmoDragging_||nurbsBoxActive_)return false;
    if(textEdit_.kind==TextEditKind::NurbsControl)cancel_text_edit();
    const bool rebuild=action=="nurbs.source_rebuild",adaptive=action=="nurbs.source_adaptive",crop=action=="nurbs.source_crop",fillet=action=="nurbs.source_fillet_corner"||action=="nurbs.source_blend_corner_radius"||action=="nurbs.source_blend_corner_setback";
    const bool attach=action=="nurbs.source_attach_c0"||action=="nurbs.source_attach_c1"||action=="nurbs.source_attach_g2",project=action=="nurbs.source_project",intersect=action=="nurbs.source_intersect_planes";
    const bool changesTrimDomain=crop||attach||intersect||action=="nurbs.source_boundaries"||action.starts_with("nurbs.source_reverse_")||action=="nurbs.source_swap_uv"||action.starts_with("nurbs.source_split_");
    if(changesTrimDomain)for(auto id:workspace_.selected_objects()){const auto* o=workspace_.document().find_object(id);if(o&&has_nurbs_source(*o)){auto f=editor_nurbs_surface_features(*o);if(f&&f->trim){set_status("This operation needs untrimmed source boundaries; untrim first or use trim-region editing",true);return false;}}}
    if(rebuild||adaptive||crop||fillet||attach||project||intersect){
        const bool pair=attach||project||intersect;const auto primary=workspace_.selected_object();auto& document=workspace_.document();auto* target=primary?document.find_object(*primary):nullptr;
        if(!target||workspace_.selection_count()!=(pair?2U:1U)){set_status(pair?"Select two sources; the active surface is the reference/target":"Select one native NURBS source",true);return false;}
        EditorObject* appearance=target;std::optional<NurbsSource> source,other;std::string error;
        for(auto id:workspace_.selected_objects()){
            auto* o=document.find_object(id);if(!o||!o->flags.visible||o->flags.locked||o->prefabLink||is_isolated_out(id)||!validate_editor_nurbs(*o,&error)){set_status(error.empty()?"Select visible unlocked owned NURBS sources":error,true);return false;}
            auto s=editor_nurbs_source(*o,&error);if(!s){set_status(error,true);return false;}
            if(id==*primary)source=*s;
            else{
                appearance=o;auto from=document.world_transform(id).value_or(o->transform),to=document.world_transform(*primary).value_or(target->transform);
                const auto inverse=conjugate(to.rotation);const auto x=rotate(inverse,rotate(from.rotation,{1,0,0})),y=rotate(inverse,rotate(from.rotation,{0,1,0})),z=rotate(inverse,rotate(from.rotation,{0,0,1}));const auto delta=rotate(inverse,subtract(from.position,to.position));
                if(target->voxelScale.x==0||target->voxelScale.y==0||target->voxelScale.z==0){set_status("Target scale must be invertible",true);return false;}
                for(auto& p:s->controls){auto q=p;p.x=(delta.x+q.x*o->voxelScale.x*x.x+q.y*o->voxelScale.y*y.x+q.z*o->voxelScale.z*z.x)/target->voxelScale.x;p.y=(delta.y+q.x*o->voxelScale.x*x.y+q.y*o->voxelScale.y*y.y+q.z*o->voxelScale.z*z.y)/target->voxelScale.y;p.z=(delta.z+q.x*o->voxelScale.x*x.z+q.y*o->voxelScale.y*y.z+q.z*o->voxelScale.z*z.z)/target->voxelScale.z;}
                other=std::move(*s);
            }
        }
        std::optional<NurbsSource> output;std::optional<CookedPolygonAsset> mesh;std::string label,status;
        const auto newId=document.allocate_object_id();
        if(rebuild){auto fit=rebuild_nurbs_curve(*source,nurbsSweepOptions_.sections,.01,&error);if(fit){output=std::move(fit->source);status="Curve rebuilt; max sampled local error "+std::to_string(fit->maxSampledError)+" (not certified)";}label="Rebuilt Curve";}
        else if(adaptive){
            auto features=editor_nurbs_surface_features(*target,&error);
            if(features&&features->trim){auto proxy=clone_editor_object(*target);proxy.id=newId;auto* component=find_component_by_type(std::span<Component>(proxy.components),kNurbsComponent);component->properties.erase(std::string(kNurbsSeamProperty));auto policy=*source;policy.adaptiveDisplay=true;policy.chordTolerance=.01;policy.normalAngle=.15;mesh=tessellate_editor_nurbs_display(proxy,policy,&error);}
            else mesh=tessellate_nurbs_adaptive(*source,newId,.01,.15,&error);
            label="Adaptive Polygon";status="Adaptive surface polygon added; sampled local tolerance 0.01, angle 0.15 rad; source retained";}
        else if(attach){
            auto options=nurbsAttachmentOptions_;options.c1=action!="nurbs.source_attach_c0";options.c2=action=="nurbs.source_attach_g2";
            auto attached=attach_nurbs_boundaries(*source,*other,options,&error);
            if(attached){output=std::move(attached->moving);const bool v=options.movingBoundary==NurbsBoundary::UMin||options.movingBoundary==NurbsBoundary::UMax;
                status="Boundary matched with compatible degree "+std::to_string(v?output->v.degree:output->u.degree)+
                    "; "+(options.c2?"C2 in normalized cross-boundary domains (curvature matches: G2)":options.c1?"C1 in normalized cross-boundary domains":"C0 position")+"; originals retained, separate patches";}
            label="Boundary Matched Patch";
        }
        else if(project){output=project_nurbs_curve(*other,*source,nurbsSweepOptions_.sections,&error);label="Projected Sample Guide";status="Projected sample guide added; connecting segments are approximate; originals retained";}
        else if(intersect){output=intersect_nurbs_planes(*source,*other,1e-8,&error);label="Planar Intersection";status="Affine patch intersection segment added; both surfaces retained";}
        else if(crop){const double f=nurbsOperationParameter_;if(!(f>0&&f<.5)){set_status("Set parameter / length fraction to a crop margin strictly between 0 and 0.5",true);return false;}output=crop_nurbs_surface(*source,std::lerp(source->u.knots.front(),source->u.knots.back(),f),std::lerp(source->u.knots.front(),source->u.knots.back(),1-f),std::lerp(source->v.knots.front(),source->v.knots.back(),f),std::lerp(source->v.knots.front(),source->v.knots.back(),1-f),&error);label="Rectangle Crop";status="Exact rectangular subpatch added; original retained; no arbitrary trim loops";}
        else{
            if(!(nurbsOperationParameter_>0&&nurbsOperationParameter_<1)){set_status("Corner leg fraction must lie strictly between 0 and 1",true);return false;}
            if(!source->is_curve()||source->columns!=3||source->u.degree!=1){set_status("Select a three-control degree-one corner curve",true);return false;}
            const auto a=source->controls[0],b=source->controls[1],c=source->controls[2];const double la=std::hypot(a.x-b.x,a.y-b.y,a.z-b.z),lc=std::hypot(c.x-b.x,c.y-b.y,c.z-b.z);const double cosine=la>0&&lc>0?((a.x-b.x)*(c.x-b.x)+(a.y-b.y)*(c.y-b.y)+(a.z-b.z)*(c.z-b.z))/(la*lc):1;const double theta=std::acos(std::clamp(cosine,-1.0,1.0));const double setback=nurbsOperationParameter_*(std::min)(la,lc),radius=setback*std::tan(theta/2);
            if(action=="nurbs.source_fillet_corner"){output=fillet_nurbs_corner(*source,radius,&error);label="Corner Fillet Arc";status="Rational circular corner arc added; trim/join legs separately; original retained";}
            else{
                // Radius mode: as round as the circular fillet this fraction makes. Setback mode: starts where that fillet starts.
                const bool matchRadius=action=="nurbs.source_blend_corner_radius";
                auto blend=blend_nurbs_corner(*source,matchRadius?NurbsCornerBlendSize::MatchRadius:NurbsCornerBlendSize::Setback,matchRadius?radius:setback,&error);
                if(blend){output=std::move(blend->source);std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(4)
                    <<"G2 corner blend added (zero curvature where it meets each leg, so no highlight line); setback "<<blend->setback<<" local units"
                    <<", peak curvature "<<blend->peakCurvature<<" vs "<<blend->arcCurvature<<" for a circular fillet through the same midpoint; trim/join legs separately; original retained";status=text.str();}
                label="G2 Corner Blend";
            }
        }
        if(!mesh&&!output){set_status(error,true);return false;}
        std::optional<EditorObject> made;
        if(output)made=make_editor_nurbs(newId,*output,&error);else{made.emplace(newId,label);made->polygon=std::move(mesh);}
        if(!made){set_status(error,true);return false;}made->name=appearance->name+" ("+label+")";made->transform=target->transform;made->voxelScale=target->voxelScale;made->voxelSizeMeters=target->voxelSizeMeters;made->parent=target->parent;made->flags=appearance->flags;made->tags=appearance->tags;made->groups=appearance->groups;made->layer=appearance->layer;
        if(output&&output->is_curve())made->flags.collisionEnabled=false;
        auto& baked=*made->polygon;auto& old=*appearance->polygon;baked.materials=old.materials;baked.materialBindings=old.materialBindings;baked.images=old.images;baked.samplers=old.samplers;baked.textures=old.textures;baked.contentHash=polygon_asset_content_hash(baked);auto valid=validate_polygon_asset(baked);if(!valid){set_status(valid.message,true);return false;}
        restore_parked_nurbs_controls();const auto before=capture_selection_snapshot();auto result=workspace_.commands().execute(document,std::make_unique<AddObjectCommand>(std::move(*made),label));
        if(!result.success){set_status(result.message,true);return false;}workspace_.select_object(newId);selectionDomain_=SelectionDomain::Objects;nurbsEditingObject_=0;nurbsSelected_.clear();nurbsControl_=0;nurbsConstructionPanel_=false;if(!viewportSettings_.isolatedObjects.empty())viewportSettings_.isolatedObjects.insert(newId);workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());set_status(status+"; one undo step",false,12);return true;
    }
    auto id=workspace_.selected_object();auto& doc=workspace_.document();auto* object=id?doc.find_object(*id):nullptr;
    if(!object||workspace_.selection_count()!=1||!object->flags.visible||object->flags.locked||object->prefabLink||is_isolated_out(object->id)){
        set_status("Select one visible unlocked native NURBS source; detach prefab instances first",true);return false;
    }
    std::string error;auto source=editor_nurbs_source(*object,&error);
    if(!source||!validate_editor_nurbs(*object,&error)){set_status(error,true);return false;}
    if(action=="nurbs.source_length"){
        auto length=measure_nurbs_curve(*source,source->u.knots[source->u.degree],source->u.knots[source->columns],1e-6,
            {double(object->voxelScale.x),double(object->voxelScale.y),double(object->voxelScale.z)},&error);
        if(!length){set_status(error,true);return false;}
        std::ostringstream message;message.imbue(std::locale::classic());
        message<<std::setprecision(10)<<"NURBS curve length: "<<length->length<<" world units (numerical error estimate "<<length->estimatedError<<"; not certified)";
        set_status(message.str(),false,15.0F);return true;
    }
    const bool elevate=action=="nurbs.source_elevate_u"||action=="nurbs.source_elevate_v";
    const bool boundaries=action=="nurbs.source_boundaries";
    const bool quality=action=="nurbs.source_quality",resetUv=action=="nurbs.source_reset_uv";
    const bool adaptiveSettings=action=="nurbs.source_display_coarse"||action=="nurbs.source_display_standard"||action=="nurbs.source_display_fine";
    const bool v=action.ends_with("_v");
    const bool reverse=action.starts_with("nurbs.source_reverse_");
    const bool transpose=action=="nurbs.source_swap_uv";
    const bool insert=action.starts_with("nurbs.source_insert_");
    const bool lengthSplit=action=="nurbs.source_split_length";
    const bool split=action.starts_with("nurbs.source_split_");
    const bool iso=action.starts_with("nurbs.source_iso_");
    if(!reverse&&!transpose&&!insert&&!split&&!iso&&!quality&&!resetUv&&!boundaries&&!elevate&&!adaptiveSettings)return false;
    if(v&&source->is_curve()){set_status("A NURBS curve has no V axis",true);return false;}
    const auto& axis=v?source->v:source->u;const auto count=v?source->rows:source->columns;
    double t=std::lerp(axis.knots[axis.degree],axis.knots[count],nurbsOperationParameter_);
    if(lengthSplit){
        if(nurbsOperationParameter_<=0||nurbsOperationParameter_>=1){set_status("A length split requires an interior fraction between 0 and 1",true);return false;}
        auto parameter=nurbs_parameter_at_length_fraction(*source,nurbsOperationParameter_,1e-6,
            {double(object->voxelScale.x),double(object->voxelScale.y),double(object->voxelScale.z)},&error);
        if(!parameter){set_status(error,true);return false;}
        t=parameter->parameter;
    }
    if(quality||resetUv||adaptiveSettings||lengthSplit)restore_parked_nurbs_controls(); // retain the selection parked by settings for undo
    else nurbsParkedObject_=0;
    const auto selection=capture_selection_snapshot();
    if(has_nurbs_seam(*object)&&(reverse||transpose)){set_status("Unlink the NURBS seam before changing degree, knots or parameter directions",true);return false;}
    if(reverse||transpose||insert||quality||resetUv||elevate||adaptiveSettings){
        auto after=(quality||resetUv||adaptiveSettings)?source:elevate?elevate_nurbs_degree(*source,v,&error):transpose?transpose_nurbs(*source,&error):reverse?reverse_nurbs_axis(*source,v,&error):insert_nurbs_knot(*source,v,t,&error);
        if(!after){set_status(error,true);return false;}
        if(quality){after->samplesPerSpan=nurbsDisplaySamples_;after->creaseNormals=true;after->adaptiveDisplay=false;}
        if(adaptiveSettings){if(source->is_curve()){set_status("Adaptive display requires a surface",true);return false;}after->adaptiveDisplay=true;after->creaseNormals=true;after->chordTolerance=action=="nurbs.source_display_coarse"?.05:action=="nurbs.source_display_fine"?.0025:.01;after->normalAngle=action=="nurbs.source_display_coarse"?.3:action=="nurbs.source_display_fine"?.075:.15;}
        if(resetUv){after->textureU={0,1};after->textureV={0,1};}
        if(*after==*source){set_status("NURBS settings already match; no history added");return true;}
        auto command=has_nurbs_seam(*object)&&(insert||elevate)?refine_nurbs_network_command(*id,v,insert?std::optional<double>(t):std::nullopt):std::unique_ptr<IEditorCommand>(std::make_unique<EditNurbsCommand>(*id,*source,*after));
        auto result=workspace_.commands().execute(doc,std::move(command));
        if(result.success){nurbsConstructionPanel_=false;if(!quality&&!resetUv&&!adaptiveSettings){nurbsSelected_.clear();nurbsControl_=0;}workspace_.commands().set_last_selection_transition(selection,capture_selection_snapshot());}
        if(result.success)refresh_nurbs_menu_state();
        set_status(result.success?(elevate?"NURBS degree elevated without refitting; reselect controls":adaptiveSettings?"Adaptive surface display saved and rebuilt; sampled local tolerances, synchronous updates":quality?"Fixed NURBS display rebuilt with crease normals and selected density":resetUv?"NURBS texture ranges reset to 0..1":"NURBS parameterization updated; reselect controls (single-axis reversal / U-V swap flips the surface normal)"):result.message,!result.success);return result.success;
    }
    std::vector<NurbsSource> results;
    if(boundaries){auto edges=extract_nurbs_boundaries(*source,&error);if(edges)for(auto& edge:*edges)results.push_back(std::move(edge));}
    else if(split){auto pieces=split_nurbs(*source,v,t,&error);if(pieces){results.push_back(std::move(pieces->first));results.push_back(std::move(pieces->second));}}
    else {auto curve=extract_nurbs_isocurve(*source,v,t,&error);if(curve)results.push_back(std::move(*curve));}
    if(results.empty()){set_status(error,true);return false;}
    // Prepare every source and bake before publishing any result to the scene.
    auto command=std::make_unique<CompoundCommand>(boundaries?"Extract NURBS boundaries":split?"Split NURBS (keep source)":"Extract NURBS isocurve");
    std::vector<EditorObjectId> created;
    for(std::size_t i=0;i<results.size();++i){
        const auto newId=doc.allocate_object_id();auto result=make_editor_nurbs(newId,results[i],&error);
        if(!result){set_status(error,true);return false;}
        constexpr const char* edgeNames[]={" (Boundary V-min)"," (Boundary U-max)"," (Boundary V-max reversed)"," (Boundary U-min reversed)"};
        result->name=object->name+(boundaries?edgeNames[i]:(split?(i==0?" (Split A)":" (Split B)"):(v?" (Fixed V curve)":" (Fixed U curve)")));
        result->transform=object->transform;result->voxelScale=object->voxelScale;result->voxelSizeMeters=object->voxelSizeMeters;
        result->flags=object->flags;if(iso||boundaries)result->flags.collisionEnabled=false;
        result->tags=object->tags;result->groups=object->groups;result->layer=object->layer;
        auto& mesh=*result->polygon;const auto& old=*object->polygon;
        mesh.materials=old.materials;mesh.materialBindings=old.materialBindings;mesh.images=old.images;mesh.samplers=old.samplers;mesh.textures=old.textures;
        mesh.contentHash=polygon_asset_content_hash(mesh);
        const auto valid=validate_polygon_asset(mesh);if(!valid){set_status(valid.message,true);return false;}
        created.push_back(newId);command->add(std::make_unique<AddObjectCommand>(std::move(*result)));
    }
    close_previews_for_voxel_edit("NURBS source operations");
    const auto result=workspace_.commands().execute(doc,std::move(command));
    if(!result.success){set_status(result.message,true);return false;}
    if(!viewportSettings_.isolatedObjects.empty())for(auto newId:created)viewportSettings_.isolatedObjects.insert(newId);
    workspace_.select_object(created.front());selectionDomain_=SelectionDomain::Objects;
    nurbsEditingObject_=0;nurbsSelected_.clear();nurbsControl_=0;nurbsConstructionPanel_=false;
    workspace_.commands().set_last_selection_transition(selection,capture_selection_snapshot());
    set_status(boundaries?"Four editable parameter-boundary curves added in one undo step; surface retained":lengthSplit?"Curve split by world-length fraction in one undo step; source retained (numerical estimate, not certified)":split?"Two editable NURBS pieces added in one undo step; original retained":"Editable rational isocurve added; original surface retained");return true;
}
}
