#include "dve/editor_native.hpp"
#include "dve/editor_nurbs_surface.hpp"
#include "dve/nurbs_modeling.hpp"
#include <algorithm>
namespace dve::editor {
bool NativeEditorController::dispatch_nurbs_surface(std::string_view action){
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active()||longJob_||gizmoDragging_||nurbsBoxActive_)return false;
    auto& doc=workspace_.document();auto id=workspace_.selected_object();auto* object=id?doc.find_object(*id):nullptr;
    if(!object||!object->flags.visible||object->flags.locked||object->prefabLink||is_isolated_out(object->id)){set_status("Select a visible unlocked owned NURBS surface",true);return false;}
    std::string error;auto source=editor_nurbs_source(*object,&error);auto features=editor_nurbs_surface_features(*object,&error);
    if(!source||source->is_curve()||!features){set_status(error.empty()?"Select a NURBS surface":error,true);return false;}
    std::unique_ptr<IEditorCommand> command;
    if(action=="nurbs.source_project_persistent"){
        if(workspace_.selection_count()!=2){set_status("Select a curve and make the target surface active",true);return false;}
        EditorObjectId other=0;for(auto selected:workspace_.selected_objects())if(selected!=*id)other=selected;
        command=project_nurbs_surface_curve_command(*id,other,std::clamp(nurbsSweepOptions_.sections,3U,257U));
    }else{
        if(workspace_.selection_count()!=1){set_status("Select one NURBS surface",true);return false;}
        const auto curve=features->curves.empty()?0:features->curves.back().id;
        if(action=="nurbs.source_trim_curve")command=trim_nurbs_surface_curve_command(*id,curve);
        else if(action=="nurbs.source_curve_refresh")command=refresh_nurbs_surface_curve_command(*id,curve);
        else if(action=="nurbs.source_untrim")command=trim_nurbs_surface_command(*id,std::nullopt);
        else if(action=="nurbs.source_trim_invert"){
            if(!features->trim){set_status("Surface has no trim loops",true);return false;}
            auto after=*features;after.trim->inverted=!after.trim->inverted;command=set_nurbs_surface_features_command(*id,*features,after);
        }else if(action=="nurbs.source_curve_remove"){
            if(!curve){set_status("Surface has no persistent curves",true);return false;}
            auto after=*features;after.curves.pop_back();if(after.trim){std::erase_if(after.trim->loops,[&](const auto& l){return l.id==curve;});if(after.trim->loops.empty())after.trim.reset();}
            command=set_nurbs_surface_features_command(*id,*features,after);
        }else if(action=="nurbs.source_trim_region"){
            const auto hit=hover_pick();if(!hit||hit->objectId!=*id){set_status("Hover the retained surface before selecting its trim region",true);return false;}
            const auto pose=doc.world_transform(*id).value_or(object->transform);auto p=inverse_transform_point(pose,hit->worldPosition);p={p.x/object->voxelScale.x,p.y/object->voxelScale.y,p.z/object->voxelScale.z};
            auto uv=project_nurbs_point(*source,{double(p.x),double(p.y),double(p.z)},1e-7,&error);if(!uv){set_status(error,true);return false;}
            command=select_nurbs_trim_region_command(*id,{(uv->u-source->u.knots.front())/(source->u.knots.back()-source->u.knots.front()),(uv->v-source->v.knots.front())/(source->v.knots.back()-source->v.knots.front())});
        }else return false;
    }
    restore_parked_nurbs_controls();const auto selection=capture_selection_snapshot();auto result=workspace_.commands().execute(doc,std::move(command));
    if(result.success){nurbsSelected_.clear();nurbsControl_=0;workspace_.commands().set_last_selection_transition(selection,capture_selection_snapshot());refresh_nurbs_menu_state();}
    set_status(result.success?"Persistent surface curves/trim updated; source retained; one undo step":result.message,!result.success,10);return result.success;
}
bool NativeEditorController::edit_nurbs_surface_uv_control(EditorObjectId surface,std::uint64_t curve,std::size_t control,NurbsUV uv){
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active()||longJob_||gizmoDragging_||nurbsBoxActive_)return false;
    auto* object=workspace_.document().find_object(surface);std::string error;if(!object||is_isolated_out(surface))return false;
    auto before=editor_nurbs_surface_features(*object,&error);if(!before){set_status(error,true);return false;}auto after=*before;
    auto found=std::find_if(after.curves.begin(),after.curves.end(),[&](const auto& c){return c.id==curve;});if(found==after.curves.end()||control>=found->uv.controls.size())return false;
    const auto first=found->uv.controls.front(),last=found->uv.controls.back();
    const bool closed=first.x==last.x&&first.y==last.y;
    found->uv.controls[control].x=uv[0];found->uv.controls[control].y=uv[1];
    if(closed&&(control==0||control+1==found->uv.controls.size())){auto& partner=control==0?found->uv.controls.back():found->uv.controls.front();partner.x=uv[0];partner.y=uv[1];}
    auto result=workspace_.commands().execute(workspace_.document(),set_nurbs_surface_features_command(surface,*before,after));
    set_status(result.success?"Surface UV control edited; trim and guides updated":result.message,!result.success);if(result.success)refresh_nurbs_menu_state();return result.success;
}
}
