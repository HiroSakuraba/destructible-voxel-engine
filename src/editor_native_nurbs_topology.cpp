#include "dve/editor_native.hpp"
#include <cmath>
namespace dve::editor {
bool NativeEditorController::dispatch_nurbs_topology(std::string_view action){
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active()||longJob_||gizmoDragging_||nurbsBoxActive_)return false;
    if(textEdit_.kind==TextEditKind::NurbsControl)cancel_text_edit();
    auto id=workspace_.selected_object();auto& doc=workspace_.document();auto* object=id?doc.find_object(*id):nullptr;
    if(!object||workspace_.selection_count()!=1||!object->flags.visible||object->flags.locked||object->prefabLink||is_isolated_out(object->id)){
        set_status("Select one visible unlocked native NURBS source; detach prefab instances first",true);return false;
    }
    std::string error;auto source=editor_nurbs_source(*object,&error);
    if(!source||!validate_editor_nurbs(*object,&error)){set_status(error,true);return false;}
    const bool quality=action=="nurbs.source_quality",resetUv=action=="nurbs.source_reset_uv";
    const bool v=action.ends_with("_v");
    const bool reverse=action.starts_with("nurbs.source_reverse_");
    const bool transpose=action=="nurbs.source_swap_uv";
    const bool insert=action.starts_with("nurbs.source_insert_");
    const bool split=action.starts_with("nurbs.source_split_");
    const bool iso=action.starts_with("nurbs.source_iso_");
    if(!reverse&&!transpose&&!insert&&!split&&!iso&&!quality&&!resetUv)return false;
    if(v&&source->is_curve()){set_status("A NURBS curve has no V axis",true);return false;}
    const auto& axis=v?source->v:source->u;const auto count=v?source->rows:source->columns;
    const double t=std::lerp(axis.knots[axis.degree],axis.knots[count],nurbsOperationParameter_);
    const auto selection=capture_selection_snapshot();
    if(reverse||transpose||insert||quality||resetUv){
        auto after=(quality||resetUv)?source:transpose?transpose_nurbs(*source,&error):reverse?reverse_nurbs_axis(*source,v,&error):insert_nurbs_knot(*source,v,t,&error);
        if(!after){set_status(error,true);return false;}
        if(quality){after->samplesPerSpan=nurbsDisplaySamples_;after->creaseNormals=true;}
        if(resetUv){after->textureU={0,1};after->textureV={0,1};}
        if(*after==*source){set_status("NURBS settings already match; no history added");return true;}
        auto result=workspace_.commands().execute(doc,std::make_unique<EditNurbsCommand>(*id,*source,*after));
        if(result.success){nurbsConstructionPanel_=false;if(!quality&&!resetUv){nurbsSelected_.clear();nurbsControl_=0;}workspace_.commands().set_last_selection_transition(selection,capture_selection_snapshot());}
        set_status(result.success?(quality?"NURBS display rebuilt with crease normals and selected density":resetUv?"NURBS texture ranges reset to 0..1":"NURBS parameterization updated; reselect controls (single-axis reversal / U-V swap flips the surface normal)"):result.message,!result.success);return result.success;
    }
    std::vector<NurbsSource> results;
    if(split){auto pieces=split_nurbs(*source,v,t,&error);if(pieces){results.push_back(std::move(pieces->first));results.push_back(std::move(pieces->second));}}
    else {auto curve=extract_nurbs_isocurve(*source,v,t,&error);if(curve)results.push_back(std::move(*curve));}
    if(results.empty()){set_status(error,true);return false;}
    // Prepare every source and bake before publishing either half to the scene.
    auto command=std::make_unique<CompoundCommand>(split?"Split NURBS (keep source)":"Extract NURBS isocurve");
    std::vector<EditorObjectId> created;
    for(std::size_t i=0;i<results.size();++i){
        const auto newId=doc.allocate_object_id();auto result=make_editor_nurbs(newId,results[i],&error);
        if(!result){set_status(error,true);return false;}
        result->name=object->name+(split?(i==0?" (Split A)":" (Split B)"):(v?" (Fixed V curve)":" (Fixed U curve)"));
        result->transform=object->transform;result->voxelScale=object->voxelScale;result->voxelSizeMeters=object->voxelSizeMeters;
        result->flags=object->flags;if(iso)result->flags.collisionEnabled=false;
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
    set_status(split?"Two editable NURBS pieces added in one undo step; original retained":"Editable rational isocurve added; original surface retained");return true;
}
}
