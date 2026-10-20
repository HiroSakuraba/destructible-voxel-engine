#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/voxel_metric.hpp"
#include <algorithm>
#include <cmath>
namespace dve::editor {
namespace {
using D3=std::array<double,3>;
D3 rotate_double(Quaternion q,D3 v){
    const double x=q.x,y=q.y,z=q.z,w=q.w,n=x*x+y*y+z*z+w*w;
    // Matches normalized quaternion rotation while retaining control precision.
    const D3 t{2*(y*v[2]-z*v[1])/n,2*(z*v[0]-x*v[2])/n,2*(x*v[1]-y*v[0])/n};
    return {v[0]+w*t[0]+y*t[2]-z*t[1],v[1]+w*t[1]+z*t[0]-x*t[2],v[2]+w*t[2]+x*t[1]-y*t[0]};
}
}
bool NativeEditorController::nurbs_control_mode() const{
    if(!nurbsEditingObject_||workspace_.selection_count()!=1||workspace_.selected_object()!=nurbsEditingObject_||
       workspace_.mode()!=EditorMode::Edit||playSession_.active()||selectionDomain_!=SelectionDomain::Objects)return false;
    auto* o=workspace_.document().find_object(nurbsEditingObject_);
    return o&&o->is_nurbs()&&o->flags.visible&&!o->flags.locked&&!o->prefabLink&&!is_isolated_out(o->id);
}
bool NativeEditorController::select_nurbs_controls(const std::set<std::size_t>& hits,SelectionOperation operation){
    if(!nurbs_control_mode()||gizmoDragging_||textEdit_.kind!=TextEditKind::Inactive)return false;
    const auto* o=workspace_.document().find_object(nurbsEditingObject_);auto source=editor_nurbs_source(*o);
    if(!source||std::any_of(hits.begin(),hits.end(),[&](auto i){return i>=source->controls.size();}))return false;
    if(operation==SelectionOperation::Replace)nurbsSelected_=hits;
    else if(operation==SelectionOperation::Intersect)std::erase_if(nurbsSelected_,[&](auto i){return !hits.contains(i);});
    else for(auto i:hits){
        if(operation==SelectionOperation::Subtract)nurbsSelected_.erase(i);
        else if(operation==SelectionOperation::Toggle&&nurbsSelected_.contains(i))nurbsSelected_.erase(i);
        else nurbsSelected_.insert(i);
    }
    if(!nurbsSelected_.contains(nurbsControl_)&&!nurbsSelected_.empty())nurbsControl_=*nurbsSelected_.begin();
    return true;
}
EditorObjectBounds NativeEditorController::nurbs_control_bounds() const{
    EditorObjectBounds bounds;if(!nurbs_control_mode())return bounds;
    const auto* o=workspace_.document().find_object(nurbsEditingObject_);auto source=editor_nurbs_source(*o);if(!source)return bounds;
    for(auto i:nurbsSelected_)if(i<source->controls.size()){
        const auto& p=source->controls[i];auto v=voxel_to_world(o->transform,o->voxelScale,{float(p.x),float(p.y),float(p.z)});
        if(!bounds.valid){bounds.minimum=bounds.maximum=v;bounds.valid=true;}
        else {bounds.minimum={std::min(bounds.minimum.x,v.x),std::min(bounds.minimum.y,v.y),std::min(bounds.minimum.z,v.z)};
            bounds.maximum={std::max(bounds.maximum.x,v.x),std::max(bounds.maximum.y,v.y),std::max(bounds.maximum.z,v.z)};}
    }
    return bounds;
}
bool NativeEditorController::begin_nurbs_gizmo(){
    if(!nurbs_control_mode()||nurbsSelected_.empty())return false;
    const auto* o=workspace_.document().find_object(nurbsEditingObject_);std::string error;
    auto s=editor_nurbs_source(*o,&error);
    if(!s||!validate_editor_nurbs(*o,&error)||std::any_of(nurbsSelected_.begin(),nurbsSelected_.end(),[&](auto i){return i>=s->controls.size();})){
        set_status(error.empty()?"Select valid NURBS controls":error,true);return false;
    }
    nurbsDragSelection_=capture_selection_snapshot();nurbsDragBefore_=s;nurbsDragPreview_=s;
    nurbsDragMeshBefore_=*o->polygon;nurbsDragValid_=true;return true;
}
bool NativeEditorController::preview_nurbs_transform(Float3 translation,Quaternion rotation,Float3 scale,Float3 pivot){
    if(!nurbsDragBefore_||!nurbsDragPreview_)return false;
    auto* o=workspace_.document().find_object(nurbsEditingObject_);std::string error;
    auto current=o?editor_nurbs_source(*o,&error):std::nullopt;
    if(!current||*current!=*nurbsDragPreview_){nurbsDragValid_=false;set_status("NURBS source changed during drag; cancel and reopen",true);return false;}
    auto after=*nurbsDragBefore_;
    const bool identity=translation.x==0&&translation.y==0&&translation.z==0&&scale.x==1&&scale.y==1&&scale.z==1&&
        rotation.x==0&&rotation.y==0&&rotation.z==0&&std::abs(rotation.w)==1;
    if(!identity)for(auto i:nurbsSelected_){
        if(i>=after.controls.size()){nurbsDragValid_=false;return false;}
        auto& p=after.controls[i];
        auto world=rotate_double(o->transform.rotation,{p.x*o->voxelScale.x,p.y*o->voxelScale.y,p.z*o->voxelScale.z});
        world={world[0]+o->transform.position.x-pivot.x,world[1]+o->transform.position.y-pivot.y,world[2]+o->transform.position.z-pivot.z};
        auto local=rotate_double(conjugate(o->transform.rotation),world);
        local={local[0]*scale.x,local[1]*scale.y,local[2]*scale.z};
        world=rotate_double(rotation,rotate_double(o->transform.rotation,local));
        world={world[0]+pivot.x+translation.x-o->transform.position.x,world[1]+pivot.y+translation.y-o->transform.position.y,world[2]+pivot.z+translation.z-o->transform.position.z};
        local=rotate_double(conjugate(o->transform.rotation),world);
        p.x=local[0]/o->voxelScale.x;p.y=local[1]/o->voxelScale.y;p.z=local[2]/o->voxelScale.z;
    }
    if(!replace_editor_nurbs_source(*o,after,&error)){nurbsDragValid_=false;set_status(error,true);return false;}
    nurbsDragValid_=true;nurbsDragPreview_=std::move(after);return true;
}
void NativeEditorController::finish_nurbs_gizmo(bool cancel){
    auto* o=workspace_.document().find_object(nurbsEditingObject_);std::string error;
    auto current=o?editor_nurbs_source(*o,&error):std::nullopt;
    if(current&&nurbsDragPreview_&&*current==*nurbsDragPreview_&&nurbsDragBefore_&&nurbsDragMeshBefore_){
        auto after=*current;
        find_component_by_type(std::span<Component>(o->components),kNurbsComponent)->properties["source"]=encode_nurbs(*nurbsDragBefore_);
        o->polygon=*nurbsDragMeshBefore_;
        if(!cancel&&nurbsDragValid_&&after!=*nurbsDragBefore_){
            auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<EditNurbsCommand>(o->id,*nurbsDragBefore_,std::move(after)));
            if(result.success)workspace_.commands().set_last_selection_transition(nurbsDragSelection_,capture_selection_snapshot());
            set_status(result.success?"Transformed selected NURBS controls":result.message,!result.success);
        } else if(!cancel&&!nurbsDragValid_)set_status("Invalid NURBS preview cancelled",true);
    } else set_status("NURBS source changed; external source preserved",true);
    nurbsDragBefore_.reset();nurbsDragPreview_.reset();nurbsDragMeshBefore_.reset();
    gizmoDragging_=false;gizmoAxis_=0;gizmoChanges_.clear();gizmoSizeChanges_.clear();transformNumeric_.clear();
    dragSnapTarget_.reset();scalePreviewBounds_.reset();nurbsDragValid_=false;
}
void NativeEditorController::capture_nurbs_selection(EditorSelectionSnapshot& snapshot) const{
    if(!nurbs_control_mode())return;
    const auto* o=workspace_.document().find_object(nurbsEditingObject_);auto s=editor_nurbs_source(*o);if(!s)return;
    snapshot.nurbsObject=nurbsEditingObject_;snapshot.nurbsControls=nurbsSelected_;snapshot.nurbsActive=nurbsControl_;snapshot.nurbsSourceStamp=nurbs_source_stamp(*s);
}
void NativeEditorController::restore_nurbs_selection(const EditorSelectionSnapshot& snapshot){
    nurbsEditingObject_=0;nurbsSelected_.clear();nurbsBoxActive_=false;
    if(!snapshot.nurbsObject||workspace_.selected_object()!=snapshot.nurbsObject)return;
    const auto* o=workspace_.document().find_object(snapshot.nurbsObject);auto s=o?editor_nurbs_source(*o):std::nullopt;
    if(!s||nurbs_source_stamp(*s)!=snapshot.nurbsSourceStamp)return;
    if(snapshot.nurbsActive>=s->controls.size()||std::any_of(snapshot.nurbsControls.begin(),snapshot.nurbsControls.end(),[&](auto i){return i>=s->controls.size();}))return;
    nurbsEditingObject_=snapshot.nurbsObject;nurbsSelected_=snapshot.nurbsControls;nurbsControl_=snapshot.nurbsActive;
}
bool NativeEditorController::nurbs_pointer_move(int x,int y){
    if(!nurbsBoxActive_)return false;
    nurbsBoxEndX_=x;nurbsBoxEndY_=y;lastPointerX_=x;lastPointerY_=y;return true;
}
bool NativeEditorController::nurbs_pointer_up(PointerButton button,int x,int y){
    if(!nurbsBoxActive_||button!=PointerButton::Primary)return false;
    nurbsBoxActive_=false;dragButton_=PointerButton::NoButton;lastPointerX_=x;lastPointerY_=y;
    if(!nurbs_control_mode())return true;
    const auto* o=workspace_.document().find_object(nurbsEditingObject_);auto s=editor_nurbs_source(*o);if(!s)return true;
    const int loX=std::min(x,nurbsBoxX_),hiX=std::max(x,nurbsBoxX_),loY=std::min(y,nurbsBoxY_),hiY=std::max(y,nurbsBoxY_);
    std::set<std::size_t> hits;
    for(std::size_t i=0;i<s->controls.size();++i){
        const auto& c=s->controls[i];auto p=project_world_to_screen(camera_,layout_.viewport,voxel_to_world(o->transform,o->voxelScale,{float(c.x),float(c.y),float(c.z)}));
        if(p.visible&&p.x>=float(loX)&&p.x<=float(hiX)&&p.y>=float(loY)&&p.y<=float(hiY))hits.insert(i);
    }
    (void)select_nurbs_controls(hits,nurbsBoxOperation_);return true;
}
}
