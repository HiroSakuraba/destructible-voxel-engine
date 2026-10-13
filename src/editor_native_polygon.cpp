#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/voxel_metric.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <queue>
#include <utility>

namespace dve::editor {
namespace {
const char* mode_name(PolygonSelectionMode mode) {
    return mode == PolygonSelectionMode::Vertex ? "vertices" : mode == PolygonSelectionMode::Edge ? "edges" : "triangle faces";
}
bool editable(const EditorObject* object) {
    return object && object->polygon && object->flags.visible && !object->flags.locked;
}
Float3 component_point(const CookedPolygonAsset& mesh, PolygonSelectionMode mode, std::uint64_t id) {
    if (mode == PolygonSelectionMode::Vertex) return mesh.vertices.at(std::size_t(id)).position;
    if (mode == PolygonSelectionMode::Edge)
        return multiply(add(mesh.vertices.at(std::size_t(id >> 32U)).position,
                            mesh.vertices.at(std::size_t(id & 0xffffffffU)).position), .5F);
    const auto i = std::size_t(id) * 3;
    return multiply(add(add(mesh.vertices[mesh.indices[i]].position, mesh.vertices[mesh.indices[i+1]].position),
                        mesh.vertices[mesh.indices[i+2]].position), 1.0F/3.0F);
}
}

bool NativeEditorController::set_polygon_mode(PolygonSelectionMode mode) {
    cancel_polygon_preview();
    if (gizmoDragging_) finish_gizmo_drag(true);
    const auto primary = workspace_.selected_object();
    const auto* object = primary ? workspace_.document().find_object(*primary) : nullptr;
    if (workspace_.mode() != EditorMode::Edit || playSession_.active() || !editable(object) || is_isolated_out(object->id)) {
        set_status("Select a visible, unlocked polygon mesh in Edit mode first", true); return false;
    }
    close_previews_for_voxel_edit("polygon component editing");
    if (selectionDomain_ != SelectionDomain::Polygons || polygonSelection_.object != object->id)
        polygonSelection_.components.clear();
    else if (polygonSelection_.mode != mode)
        polygonSelection_ = convert_polygon_selection(*object->polygon,polygonSelection_,mode);
    polygonSelection_.object = object->id;
    polygonSelection_.mode = mode;
    workspace_.select_object(object->id);
    selectionDomain_ = SelectionDomain::Polygons;
    marquee_ = {};
    focusRegion_ = EditorFocusRegion::Viewport;
    set_active_tool(EditorToolId::Select);
    prune_polygon_selection();
    set_status(std::string(object->subdivision ? "Selecting subdivision cage " : "Selecting polygon ") + mode_name(mode) + ": click/box (left to right: window; right to left: crossing); Shift add, Ctrl toggle, Ctrl+Shift intersect");
    refresh_menu_state(); return true;
}

void NativeEditorController::prune_polygon_selection() {
    if (selectionDomain_ != SelectionDomain::Polygons) return;
    const auto* object = workspace_.document().find_object(polygonSelection_.object);
    if (!editable(object) || is_isolated_out(object->id) || workspace_.selected_object() != object->id) {
        cancel_polygon_preview();
        if (gizmoDragging_) finish_gizmo_drag(true);
        polygonSelection_.components.clear(); selectionDomain_ = SelectionDomain::Objects; return;
    }
    auto all = polygon_all_components(*object->polygon, polygonSelection_.mode);
    if (object->subdivision && polygonSelection_.mode==PolygonSelectionMode::Edge)
        all=subdivision_control_edges(*object->polygon);
    std::erase_if(polygonSelection_.components, [&](auto id) { return !all.contains(id); });
}

void NativeEditorController::select_polygon_components(const std::set<std::uint64_t>& inputHits, SelectionOperation operation) {
    if (polygonPreviewBefore_ || gizmoDragging_) return;
    prune_polygon_selection();
    const auto* object = workspace_.document().find_object(polygonSelection_.object);
    if (selectionDomain_ != SelectionDomain::Polygons || !editable(object)) return;
    auto all = polygon_all_components(*object->polygon, polygonSelection_.mode);
    auto hits=inputHits;
    if (object->subdivision) {
        if (polygonSelection_.mode==PolygonSelectionMode::Edge) all=subdivision_control_edges(*object->polygon);
        if (polygonSelection_.mode==PolygonSelectionMode::Face)
            for (const auto& face : subdivision_control_face_triangles(*object->polygon))
                if (std::any_of(face.begin(),face.end(),[&](auto id){return hits.contains(id);}))
                    hits.insert(face.begin(),face.end());
    }
    auto& selected = polygonSelection_.components;
    if (operation == SelectionOperation::Replace) selected.clear();
    if (operation == SelectionOperation::Intersect) std::erase_if(selected, [&](auto id) { return !hits.contains(id); });
    else for (auto id : hits) if (all.contains(id)) {
        if (operation == SelectionOperation::Subtract) selected.erase(id);
        else if (operation == SelectionOperation::Toggle && selected.contains(id)) selected.erase(id);
        else selected.insert(id);
    }
    std::size_t count=selected.size();
    if (object->subdivision && polygonSelection_.mode==PolygonSelectionMode::Face) {
        count=0;
        for (const auto& face : subdivision_control_face_triangles(*object->polygon))
            if (std::any_of(face.begin(),face.end(),[&](auto id){return selected.contains(id);})) ++count;
    }
    set_status(std::to_string(count)+(object->subdivision ? " subdivision control " : " polygon ")+
        (object->subdivision && polygonSelection_.mode==PolygonSelectionMode::Face ? "faces" : mode_name(polygonSelection_.mode))+" selected");
    refresh_menu_state();
}

bool NativeEditorController::polygon_component_visible(std::uint64_t id) const {
    const auto* object = workspace_.document().find_object(polygonSelection_.object);
    if (!object || !object->polygon) return false;
    const auto world = voxel_to_world(object->transform, object->voxelScale,
        component_point(*object->polygon, polygonSelection_.mode, id));
    const auto screen = project_world_to_screen(camera_, layout_.viewport, world);
    if (!screen.visible) return false;
    const auto ray = make_viewport_ray(camera_, layout_.viewport, screen.x, screen.y);
    const float distance = length(subtract(world, ray.origin));
    const auto hits = pick_editor_document_all(workspace_.document(), ray, distance + .002F, &viewportSettings_.isolatedObjects);
    if (!hits.empty() && hits.front().objectId==object->id && object->subdivision) {
        const auto cageHit=pick_editor_polygon(*object,ray,distance+.002F,true);
        return !cageHit || cageHit->distance>=distance-.002F;
    }
    return hits.empty() || hits.front().worldDistance >= distance - .002F;
}

void NativeEditorController::finish_polygon_marquee(int x, int y) {
    const auto marquee = marquee_; marquee_ = {};
    prune_polygon_selection();
    const auto* object = workspace_.document().find_object(polygonSelection_.object);
    if (!editable(object) || selectionDomain_ != SelectionDomain::Polygons) return;
    std::set<std::uint64_t> hits;
    const int dx = x-marquee.startX, dy = y-marquee.startY;
    const bool click = std::hypot(double(dx),double(dy)) <= 5.0;
    if (click) {
        const auto hit = pick_polygon_component(*object, camera_, layout_.viewport, marquee.startX, marquee.startY,
                                                polygonSelection_.mode, viewportSettings_.xraySelection);
        if (hit) hits.insert(*hit);
    } else {
        const UiRect rect{std::min(x,marquee.startX), std::min(y,marquee.startY), std::abs(dx)+1, std::abs(dy)+1};
        hits = polygon_components_in_rect(*object,camera_,layout_.viewport,rect,polygonSelection_.mode,viewportSettings_.xraySelection,x < marquee.startX,
            &workspace_.document(),&viewportSettings_.isolatedObjects);
    }
    if (click && !viewportSettings_.xraySelection) std::erase_if(hits,[&](auto id) { return !polygon_component_visible(id); });
    if (!click && x>=marquee.startX && object->subdivision && polygonSelection_.mode==PolygonSelectionMode::Face)
        for (const auto& face : subdivision_control_face_triangles(*object->polygon))
            if (!std::all_of(face.begin(),face.end(),[&](auto id){return hits.contains(id);}))
                for (auto id : face) hits.erase(id);
    select_polygon_components(hits,marquee.operation);
}

Float3 NativeEditorController::polygon_selection_centre() const {
    const auto* object = workspace_.document().find_object(polygonSelection_.object);
    if (!object || !object->polygon || polygonSelection_.components.empty()) return object ? object->transform.position : Float3{};
    // Count each geometric corner once, even when it has split UV/normal copies.
    const auto vertices = polygon_selected_vertices(*object->polygon,polygonSelection_);
    std::set<std::tuple<float,float,float>> points;
    Float3 sum{};
    for (auto i : vertices) {
        const auto p = object->polygon->vertices[i].position;
        if (points.emplace(p.x,p.y,p.z).second) sum = add(sum,voxel_to_world(object->transform,object->voxelScale,p));
    }
    return points.empty() ? object->transform.position : multiply(sum,1.0F/float(points.size()));
}

bool NativeEditorController::apply_polygon_edit(PolygonEdit operation, const PolygonEditSettings& settings) {
    if (polygonPreviewBefore_ || gizmoDragging_ || longJob_ || workspace_.mode() != EditorMode::Edit || playSession_.active()) return false;
    prune_polygon_selection();
    auto* object = workspace_.document().find_object(polygonSelection_.object);
    if (selectionDomain_ != SelectionDomain::Polygons || !editable(object)) return false;
    auto edited = edit_polygon(*object->polygon,polygonSelection_,operation,settings);
    if (!edited) { set_status(edited.error,true); return false; }
    close_previews_for_voxel_edit("polygon edit");
    const auto before = capture_selection_snapshot();
    const auto id = object->id;
    std::unique_ptr<IEditorCommand> command;
    if (edited.mesh.indices.empty()) command = std::make_unique<RemoveObjectsCommand>(std::vector<EditorObjectId>{id},"Delete polygon mesh");
    else command = std::make_unique<ReplacePolygonCommand>(id,*object->polygon,std::move(edited.mesh),"Edit polygon components");
    const auto result = workspace_.commands().execute(workspace_.document(),std::move(command));
    if (result.success) {
        polygonSelection_ = std::move(edited.selection);
        if (!workspace_.document().find_object(id)) { workspace_.clear_selection(); selectionDomain_ = SelectionDomain::Objects; }
        workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
        recompute_layout();
    }
    set_status(result.success ? "Edited polygon components" : result.message,!result.success);
    refresh_menu_state(); return result.success;
}

bool NativeEditorController::transform_polygon_components(Float3 translation, Quaternion rotation, Float3 scale, Float3 worldPivot) {
    if (polygonPreviewBefore_ || longJob_ || workspace_.mode() != EditorMode::Edit || playSession_.active()) return false;
    auto* object = workspace_.document().find_object(polygonSelection_.object);
    if (selectionDomain_ != SelectionDomain::Polygons || !editable(object) || polygonSelection_.components.empty() ||
        !polygon_selection_valid(*object->polygon,polygonSelection_) || !valid_voxel_metric(scale)) return false;
    const auto finite=[](Float3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); };
    if (!finite(translation)||!finite(worldPivot)||!std::isfinite(rotation.x)||!std::isfinite(rotation.y)||
        !std::isfinite(rotation.z)||!std::isfinite(rotation.w)) return false;
    if (length_squared(translation)==0 && scale.x==1 && scale.y==1 && scale.z==1 &&
        rotation.x==0 && rotation.y==0 && rotation.z==0 && std::abs(rotation.w)==1) return true;
    const auto before = capture_selection_snapshot();
    auto mesh = *object->polygon;
    const auto vertices = polygon_selected_vertices(mesh,polygonSelection_);
    for (auto index : vertices) {
        auto world = voxel_to_world(object->transform,object->voxelScale,mesh.vertices[index].position);
        // Scale in the same local frame used by the Scale gizmo; rotate around a world pivot.
        auto offset = rotate(conjugate(object->transform.rotation),subtract(world,worldPivot));
        offset = rotate(object->transform.rotation,multiply_components(offset,scale));
        world = add(add(worldPivot,rotate(rotation,offset)),translation);
        mesh.vertices[index].position = world_to_voxel(object->transform,object->voxelScale,world);
    }
    refresh_editor_polygon(mesh,true);
    if (!validate_polygon_asset(mesh)) { set_status("Transform produced invalid polygon geometry",true); return false; }
    if (gizmoDragging_) { object->polygon = std::move(mesh); return true; }
    const auto result = workspace_.commands().execute(workspace_.document(),
        std::make_unique<ReplacePolygonCommand>(object->id,*object->polygon,std::move(mesh),"Transform polygon components"));
    if (result.success) workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
    set_status(result.success ? "Transformed polygon components" : result.message,!result.success);
    return result.success;
}

bool NativeEditorController::begin_polygon_preview(PolygonEdit operation) {
    cancel_polygon_preview();
    prune_polygon_selection();
    auto* object = workspace_.document().find_object(polygonSelection_.object);
    if (workspace_.mode()!=EditorMode::Edit || playSession_.active() || selectionDomain_!=SelectionDomain::Polygons ||
        !editable(object) || polygonSelection_.components.empty()) return false;
    if ((operation==PolygonEdit::Extrude||operation==PolygonEdit::Inset||operation==PolygonEdit::Bevel||operation==PolygonEdit::ProjectUV) &&
        polygonSelection_.mode!=PolygonSelectionMode::Face) { set_status("Select triangle faces for this tool",true); return false; }
    if (gizmoDragging_) finish_gizmo_drag(true);
    close_previews_for_voxel_edit("polygon tool preview");
    polygonPreviewBefore_ = *object->polygon;
    polygonPreviewSelection_ = polygonSelection_;
    polygonPreviewOperation_ = operation;
    polygonPreviewSettings_ = {};
    polygonPreviewTyped_.clear();
    update_polygon_preview(); return true;
}

bool NativeEditorController::begin_polygon_slice() {
    cancel_polygon_preview();
    if (gizmoDragging_) finish_gizmo_drag(true);
    const auto primary=workspace_.selected_object();
    const auto* object=primary?workspace_.document().find_object(*primary):nullptr;
    if (!editable(object)||workspace_.mode()!=EditorMode::Edit||playSession_.active()||object->prefabLink||!object->components.empty()) {
        set_status("Slice needs an unlocked polygon mesh without components or prefab links",true); return false;
    }
    close_previews_for_voxel_edit("polygon Slice");
    polygonPreviewSelection_=polygonSelection_;
    polygonPreviewSelection_.object=object->id;
    polygonPreviewBefore_=*object->polygon;
    polygonSlicePreview_=true; polygonSliceOutput_=0; polygonSliceCap_=true; polygonSliceBack_.reset();
    polygonSliceOrigin_=voxel_to_world(object->transform,object->voxelScale,multiply(add(object->polygon->bounds.minimum,object->polygon->bounds.maximum),.5F));
    polygonSlicePoint_=polygonSliceOrigin_; polygonSliceNormal_={1,0,0}; polygonPreviewTyped_.clear();
    update_polygon_preview(); return true;
}

void NativeEditorController::update_polygon_preview() {
    if (!polygonPreviewBefore_) return;
    auto* object = workspace_.document().find_object(polygonPreviewSelection_.object);
    if (!editable(object)) { cancel_polygon_preview(); return; }
    *object->polygon = *polygonPreviewBefore_;
    polygonSelection_ = polygonPreviewSelection_;
    if (polygonSlicePreview_) {
        float offset=0;
        if (!polygonPreviewTyped_.empty()) {
            char* end{}; offset=std::strtof(polygonPreviewTyped_.c_str(),&end);
            if (end!=polygonPreviewTyped_.c_str()+polygonPreviewTyped_.size()||!std::isfinite(offset)) {
                polygonPreviewValid_=false; set_status("Slice offset must be finite",true); return;
            }
        }
        polygonSlicePoint_=add(polygonSliceOrigin_,multiply(polygonSliceNormal_,offset));
        const auto point=world_to_voxel(object->transform,object->voxelScale,polygonSlicePoint_);
        const auto normal=normalize(multiply_components(object->voxelScale,rotate(conjugate(object->transform.rotation),polygonSliceNormal_)));
        const auto front=slice_polygon(*polygonPreviewBefore_,point,normal,polygonSliceOutput_!=1,polygonSliceCap_);
        polygonSliceBack_.reset();
        polygonPreviewValid_=bool(front)&&!front.mesh.indices.empty();
        if (polygonSliceOutput_==2 && polygonPreviewValid_) {
            const auto back=slice_polygon(*polygonPreviewBefore_,point,normal,false,polygonSliceCap_);
            polygonPreviewValid_=bool(back)&&!back.mesh.indices.empty();
            if (polygonPreviewValid_) polygonSliceBack_=back.mesh;
            else set_status(back.error.empty()?"Slice leaves an empty part":back.error,true);
        }
        if (polygonPreviewValid_) { *object->polygon=front.mesh; polygonSelection_.components.clear(); }
        else set_status(front.error.empty()?"Slice preview is empty or invalid":front.error,true);
        refresh_menu_state(); return;
    }
    auto settings = polygonPreviewSettings_;
    if (!polygonPreviewTyped_.empty()) {
        char* end{}; const float value = std::strtof(polygonPreviewTyped_.c_str(),&end);
        if (end != polygonPreviewTyped_.c_str()+polygonPreviewTyped_.size() || !std::isfinite(value)) {
            polygonPreviewValid_=false; set_status("Enter a finite tool value",true); return;
        }
        if (polygonPreviewOperation_==PolygonEdit::Inset) settings.inset=value;
        else settings.amount=value;
    }
    // Face bevel exposes both height (typed) and inset (arrow keys).
    const auto edited=edit_polygon(*polygonPreviewBefore_,polygonPreviewSelection_,polygonPreviewOperation_,settings);
    polygonPreviewValid_=bool(edited);
    if (edited) { *object->polygon=edited.mesh; polygonSelection_=edited.selection; }
    else { polygonSelection_=polygonPreviewSelection_; set_status(edited.error,true); }
    refresh_menu_state();
}

void NativeEditorController::cancel_polygon_preview() {
    if (!polygonPreviewBefore_) return;
    if (auto* object=workspace_.document().find_object(polygonPreviewSelection_.object)) object->polygon=*polygonPreviewBefore_;
    polygonSelection_=polygonPreviewSelection_;
    polygonPreviewBefore_.reset(); polygonPreviewTyped_.clear(); polygonPreviewValid_=false;
    polygonSlicePreview_=false; polygonSliceBack_.reset();
}

bool NativeEditorController::commit_polygon_preview() {
    if (!polygonPreviewBefore_ || !polygonPreviewValid_) { set_status("Fix the polygon preview before applying it",true); return false; }
    auto* object=workspace_.document().find_object(polygonPreviewSelection_.object);
    if (!editable(object) || workspace_.mode()!=EditorMode::Edit) { cancel_polygon_preview(); return false; }
    auto mesh=*object->polygon; const auto selection=polygonSelection_;
    const bool slice=polygonSlicePreview_; auto back=polygonSliceBack_;
    cancel_polygon_preview();
    const auto before=capture_selection_snapshot();
    auto command=std::make_unique<CompoundCommand>(slice?"Slice polygon mesh":"Apply polygon tool");
    command->add(std::make_unique<ReplacePolygonCommand>(object->id,*object->polygon,std::move(mesh),slice?"Slice polygon geometry":"Apply polygon tool"));
    std::optional<EditorObjectId> partId;
    if (back) {
        const auto id=workspace_.document().allocate_object_id(); partId=id;
        EditorObject part(id,object->name+" (slice back)"); part.polygon=std::move(*back); part.polygon->objectId=id;
        refresh_editor_polygon(*part.polygon); part.transform=object->transform; part.voxelScale=object->voxelScale; part.subdivision=object->subdivision;
        part.flags=object->flags; part.parent=object->parent;
        command->add(std::make_unique<AddObjectCommand>(std::move(part),"Add sliced polygon part"));
    }
    const auto result=workspace_.commands().execute(workspace_.document(),std::move(command));
    if (result.success) {
        polygonSelection_=selection;
        if (slice) {
            selectionDomain_=SelectionDomain::Objects;
            workspace_.select_object(object->id);
            if (partId) { workspace_.add_to_selection(*partId); if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(*partId); }
        }
        workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
        recompute_layout();
    }
    set_status(result.success ? "Applied polygon tool" : result.message,!result.success);
    refresh_menu_state(); return result.success;
}

bool NativeEditorController::handle_polygon_preview_key(std::string_view key) {
    if (!polygonPreviewBefore_) return false;
    if (key=="escape") { cancel_polygon_preview(); refresh_menu_state(); return true; }
    if (key=="return"||key=="enter") { (void)commit_polygon_preview(); return true; }
    if (polygonSlicePreview_ && (key=="f"||key=="b"||key=="s"||key=="c"||key=="x"||key=="y"||key=="z")) {
        if (key=="f") polygonSliceOutput_=0; else if (key=="b") polygonSliceOutput_=1; else if (key=="s") polygonSliceOutput_=2;
        else if (key=="c") polygonSliceCap_=!polygonSliceCap_;
        else polygonSliceNormal_=key=="x"?Float3{1,0,0}:key=="y"?Float3{0,1,0}:Float3{0,0,1};
        update_polygon_preview(); return true;
    }
    if (key=="backspace") { if (!polygonPreviewTyped_.empty()) polygonPreviewTyped_.pop_back(); }
    else if (key=="x"||key=="y"||key=="z") polygonPreviewSettings_.uvAxis=key=="x"?0U:key=="y"?1U:2U;
    else if (key=="up"||key=="down") {
        const float step=key=="up"?.01F:-.01F;
        if (polygonPreviewOperation_==PolygonEdit::Inset||polygonPreviewOperation_==PolygonEdit::Bevel)
            polygonPreviewSettings_.inset=std::clamp(polygonPreviewSettings_.inset+step,0.0F,.99F);
        else polygonPreviewSettings_.amount+=step;
        polygonPreviewTyped_.clear();
    } else if (key.size()==1 && ((key[0]>='0'&&key[0]<='9')||key[0]=='.'||key[0]=='-')) {
        if (polygonPreviewTyped_.size()<16) polygonPreviewTyped_+=key;
    } else if (key=="period") polygonPreviewTyped_+='.';
    else if (key=="minus") polygonPreviewTyped_+='-';
    update_polygon_preview(); return true;
}

std::vector<std::string> NativeEditorController::polygon_preview_lines() const {
    if (!polygonPreviewBefore_) return {};
    if (polygonSlicePreview_) return {"Polygon Slice",std::string("World axis: ")+(polygonSliceNormal_.x?"X":polygonSliceNormal_.y?"Y":"Z")+" | offset m: "+(polygonPreviewTyped_.empty()?"0":polygonPreviewTyped_),
        std::string("Output: ")+(polygonSliceOutput_==0?"Front":polygonSliceOutput_==1?"Back":"Separate")+" | caps: "+(polygonSliceCap_?"on":"off"),
        "X/Y/Z axis | F/B/S output | C caps",polygonPreviewValid_?"Enter apply | Esc cancel":"Invalid preview | Esc cancel"};
    const char* tool=polygonPreviewOperation_==PolygonEdit::Extrude?"Extrude":polygonPreviewOperation_==PolygonEdit::Inset?"Inset":
        polygonPreviewOperation_==PolygonEdit::Bevel?"Face bevel":polygonPreviewOperation_==PolygonEdit::Weld?"Weld by distance":"Planar UV projection";
    const auto value=polygonPreviewOperation_==PolygonEdit::Inset?polygonPreviewSettings_.inset:polygonPreviewSettings_.amount;
    return {tool,std::string("Value: ")+(polygonPreviewTyped_.empty()?std::to_string(value):polygonPreviewTyped_),
        "Inset: "+std::to_string(polygonPreviewSettings_.inset),
        "Type value; arrows adjust; X/Y/Z UV axis",polygonPreviewValid_?"Enter apply | Esc cancel":"Invalid preview | Esc cancel"};
}

bool NativeEditorController::join_selected_polygons() {
    cancel_polygon_preview();
    if (gizmoDragging_) finish_gizmo_drag(true);
    const auto primary=workspace_.selected_object();
    if (!primary||workspace_.selection_count()<2||workspace_.mode()!=EditorMode::Edit||playSession_.active()) return false;
    std::vector<EditorObjectId> ids(workspace_.selected_objects().begin(),workspace_.selected_objects().end()),remove;
    for (auto id:ids) {
        const auto* object=workspace_.document().find_object(id);
        if (!editable(object)||is_isolated_out(id)||object->prefabLink||!object->components.empty()) {
            set_status("Join needs unlocked polygon objects without components or prefab links",true); return false;
        }
        // Refuse hierarchy operands: RemoveObjectsCommand also removes descendants.
        if (id!=*primary) for (const auto& [childId,child]:workspace_.document().objects()) {
            (void)childId;
            if (child.parent==id) { set_status("Reparent operand children before joining polygon objects",true); return false; }
        }
        if (id!=*primary) remove.push_back(id);
    }
    const auto joined=join_editor_polygons(workspace_.document(),*primary,ids);
    if (!joined) { set_status(joined.error,true); return false; }
    const auto before=capture_selection_snapshot();
    auto command=std::make_unique<CompoundCommand>("Join polygon meshes");
    command->add(std::make_unique<ReplacePolygonCommand>(*primary,*workspace_.document().find_object(*primary)->polygon,joined.mesh,"Join polygon geometry"));
    command->add(std::make_unique<RemoveObjectsCommand>(remove,"Remove joined operands"));
    const auto result=workspace_.commands().execute(workspace_.document(),std::move(command));
    if (result.success) { workspace_.select_object(*primary); selectionDomain_=SelectionDomain::Objects; polygonSelection_={};
        workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot()); recompute_layout(); }
    set_status(result.success?"Joined polygon meshes":result.message,!result.success); return result.success;
}

bool NativeEditorController::separate_selected_polygon() {
    cancel_polygon_preview();
    if (gizmoDragging_) finish_gizmo_drag(true);
    const auto primary=workspace_.selected_object();
    auto* object=primary?workspace_.document().find_object(*primary):nullptr;
    if (!editable(object)||workspace_.mode()!=EditorMode::Edit||playSession_.active()||object->prefabLink||!object->components.empty()) return false;
    const auto parts=separate_polygon_components(*object->polygon);
    if (parts.size()<2) { set_status("This polygon mesh has one connected shell",true); return false; }
    const auto before=capture_selection_snapshot();
    auto command=std::make_unique<CompoundCommand>("Separate polygon shells");
    command->add(std::make_unique<ReplacePolygonCommand>(object->id,*object->polygon,parts[0],"Keep first polygon shell"));
    std::vector<EditorObjectId> ids{object->id};
    for (std::size_t i=1;i<parts.size();++i) {
        const auto id=workspace_.document().allocate_object_id(); EditorObject part(id,object->name+" (shell "+std::to_string(i+1)+")");
        part.polygon=parts[i]; part.polygon->objectId=id; refresh_editor_polygon(*part.polygon);
        part.transform=object->transform; part.voxelScale=object->voxelScale; part.subdivision=object->subdivision; part.flags=object->flags; part.parent=object->parent;
        command->add(std::make_unique<AddObjectCommand>(std::move(part),"Add polygon shell")); ids.push_back(id);
    }
    const auto result=workspace_.commands().execute(workspace_.document(),std::move(command));
    if (result.success) { workspace_.clear_selection(); for (auto id:ids) { workspace_.add_to_selection(id); if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id); }
        selectionDomain_=SelectionDomain::Objects; polygonSelection_={}; workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot()); recompute_layout(); }
    set_status(result.success?"Separated polygon shells":result.message,!result.success); return result.success;
}

bool NativeEditorController::extract_selected_polygon() {
    if (polygonPreviewBefore_||gizmoDragging_||workspace_.mode()!=EditorMode::Edit||playSession_.active()) return false;
    auto* object=workspace_.document().find_object(polygonSelection_.object);
    if (selectionDomain_!=SelectionDomain::Polygons||!editable(object)||polygonSelection_.mode!=PolygonSelectionMode::Face||polygonSelection_.components.empty()) return false;
    const auto mesh=extract_polygon_faces(*object->polygon,polygonSelection_);
    const auto remaining=edit_polygon(*object->polygon,polygonSelection_,PolygonEdit::Delete);
    if (!remaining||remaining.mesh.indices.empty()) { set_status("Extract part of a mesh; use object mode for the whole mesh",true); return false; }
    const auto before=capture_selection_snapshot();
    const auto id=workspace_.document().allocate_object_id(); EditorObject part(id,object->name+" (extract)");
    part.polygon=mesh; part.polygon->objectId=id; refresh_editor_polygon(*part.polygon);
    part.transform=object->transform; part.voxelScale=object->voxelScale; part.subdivision=object->subdivision; part.flags=object->flags; part.parent=object->parent;
    auto command=std::make_unique<CompoundCommand>("Extract polygon faces");
    command->add(std::make_unique<ReplacePolygonCommand>(object->id,*object->polygon,remaining.mesh,"Remove extracted faces"));
    command->add(std::make_unique<AddObjectCommand>(std::move(part),"Add extracted polygon"));
    const auto result=workspace_.commands().execute(workspace_.document(),std::move(command));
    if (result.success) { if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
        workspace_.select_object(id); polygonSelection_={id,PolygonSelectionMode::Face,polygon_all_components(mesh,PolygonSelectionMode::Face)};
        workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot()); recompute_layout(); }
    set_status(result.success?"Extracted polygon faces":result.message,!result.success); return result.success;
}

bool NativeEditorController::handle_polygon_action(std::string_view action) {
    if (action=="polygon.vertex_mode") return set_polygon_mode(PolygonSelectionMode::Vertex);
    if (action=="polygon.edge_mode") return set_polygon_mode(PolygonSelectionMode::Edge);
    if (action=="polygon.face_mode") return set_polygon_mode(PolygonSelectionMode::Face);
    if (action=="polygon.object_mode"||(action=="polygon.component_toggle"&&selectionDomain_==SelectionDomain::Polygons)) {
        set_selection_domain(SelectionDomain::Objects); return true;
    }
    if (action=="polygon.component_toggle") return set_polygon_mode(PolygonSelectionMode::Face);
    if (action=="polygon.slice") return begin_polygon_slice();
    if (action=="polygon.join") return join_selected_polygons();
    if (action=="polygon.separate") return separate_selected_polygon();
    if (action=="polygon.extract") return extract_selected_polygon();
    if (action=="polygon.preview_apply") return commit_polygon_preview();
    if (action=="polygon.preview_cancel") { cancel_polygon_preview(); refresh_menu_state(); return true; }
    if (action=="polygon.extrude") return begin_polygon_preview(PolygonEdit::Extrude);
    if (action=="polygon.inset") return begin_polygon_preview(PolygonEdit::Inset);
    if (action=="polygon.bevel") return begin_polygon_preview(PolygonEdit::Bevel);
    if (action=="polygon.weld") return begin_polygon_preview(PolygonEdit::Weld);
    if (action=="polygon.uv_tools") return begin_polygon_preview(PolygonEdit::ProjectUV);
    if (action=="polygon.assign_material") {
        auto* object=workspace_.document().find_object(polygonSelection_.object);
        const auto* entry=materials_.find(activeMaterial_);
        if (!editable(object)||!entry||selectionDomain_!=SelectionDomain::Polygons||polygonSelection_.mode!=PolygonSelectionMode::Face||polygonSelection_.components.empty()) return false;
        auto mesh=*object->polygon;
        PolygonEditSettings settings; settings.material=std::uint32_t(mesh.materials.size());
        mesh.materials.push_back(entry->definition); mesh.materialBindings.emplace_back(); refresh_editor_polygon(mesh);
        auto edited=edit_polygon(mesh,polygonSelection_,PolygonEdit::AssignMaterial,settings);
        if (!edited) { set_status(edited.error,true); return false; }
        const auto before=capture_selection_snapshot();
        const auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<ReplacePolygonCommand>(
            object->id,*object->polygon,std::move(edited.mesh),"Assign polygon face material"));
        if (result.success) workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
        set_status(result.success?"Assigned active material to selected faces":result.message,!result.success); return result.success;
    }
    if (action=="polygon.recalculate_normals") return apply_polygon_edit(PolygonEdit::RecalculateNormals);
    if (action=="polygon.flip_normals") return apply_polygon_edit(PolygonEdit::FlipNormals);
    if (selectionDomain_!=SelectionDomain::Polygons) return false;
    prune_polygon_selection();
    const auto* object=workspace_.document().find_object(polygonSelection_.object);
    if (!editable(object)) return false;
    if (action=="edit.select_all"||action=="polygon.select_invert"||action=="polygon.select_none") {
        auto hits=polygon_all_components(*object->polygon,polygonSelection_.mode);
        if (action=="polygon.select_none") hits.clear();
        else if (action=="polygon.select_invert") for (auto id:polygonSelection_.components) hits.erase(id);
        select_polygon_components(hits,SelectionOperation::Replace); return true;
    }
    if (action=="polygon.select_shell" || action=="polygon.select_grow" || action=="polygon.select_shrink") {
        const auto operation = action=="polygon.select_grow" ? PolygonSelectionEdit::Grow
            : action=="polygon.select_shrink" ? PolygonSelectionEdit::Shrink : PolygonSelectionEdit::Connected;
        auto hits = edit_polygon_selection(*object->polygon,polygonSelection_,operation);
        if (object->subdivision && action=="polygon.select_shrink" && polygonSelection_.mode==PolygonSelectionMode::Face)
            for (const auto& face : subdivision_control_face_triangles(*object->polygon))
                if (!std::all_of(face.begin(),face.end(),[&](auto id){return hits.contains(id);}))
                    for (auto id : face) hits.erase(id);
        select_polygon_components(hits,SelectionOperation::Replace);
        return true;
    }
    if (action=="edit.delete") return apply_polygon_edit(PolygonEdit::Delete);
    if (action=="edit.duplicate") { PolygonEditSettings settings; settings.vector={.1F,0,0}; return apply_polygon_edit(PolygonEdit::Duplicate,settings); }
    if (action=="edit.copy"||action=="edit.cut") {
        if (polygonSelection_.mode!=PolygonSelectionMode::Face||polygonSelection_.components.empty()) { set_status("Select triangle faces to copy/cut",true); return false; }
        auto mesh=extract_polygon_faces(*object->polygon,polygonSelection_);
        const auto pose=object->transform; const auto scale=object->voxelScale; const auto subdivision=object->subdivision;
        if (action=="edit.cut"&&!apply_polygon_edit(PolygonEdit::Delete)) return false;
        polygonClipboard_=std::move(mesh); polygonClipboardPose_=pose; polygonClipboardScale_=scale;
        polygonClipboardSubdivision_=subdivision;
        polygonClipboardNewest_=true; voxelClipboardNewest_=false; return true;
    }
    return false;
}

namespace {
const std::map<std::string_view,std::string_view>& subdivision_tools() {
    static const std::map<std::string_view,std::string_view> tools{
        {"subdiv.vertex_mode","polygon.vertex_mode"},
        {"subdiv.edge_mode","polygon.edge_mode"},
        {"subdiv.face_mode","polygon.face_mode"},
        {"subdiv.object_mode","polygon.object_mode"},
        {"subdiv.move","transform.translate"},
        {"subdiv.rotate","transform.rotate"},
        {"subdiv.scale","transform.scale"},
        {"subdiv.select_all","edit.select_all"},
        {"subdiv.select_grow","polygon.select_grow"},
        {"subdiv.select_shrink","polygon.select_shrink"},
        {"subdiv.select_connected","polygon.select_shell"},
        {"subdiv.select_invert","polygon.select_invert"},
        {"subdiv.select_none","polygon.select_none"},
        {"subdiv.extrude","polygon.extrude"},
        {"subdiv.inset","polygon.inset"},
        {"subdiv.bevel","polygon.bevel"},
        {"subdiv.bevel_edges","mesh.bevel_edges"},
        {"subdiv.weld","polygon.weld"},
        {"subdiv.split_edges","mesh.split_edges"},
        {"subdiv.subdivide_cage","mesh.subdivide"},
        {"subdiv.bridge","mesh.bridge"},
        {"subdiv.cap","mesh.cap"},
        {"subdiv.extract","polygon.extract"},
        {"subdiv.join","polygon.join"},
        {"subdiv.separate","polygon.separate"},
        {"subdiv.slice","polygon.slice"},
        {"subdiv.assign_material","polygon.assign_material"},
        {"subdiv.uv_editor","mesh.uv_editor"},
        {"subdiv.uv_mark_seam","mesh.uv_mark_seam"},
        {"subdiv.uv_clear_seam","mesh.uv_clear_seam"},
        {"subdiv.uv_toggle_pin","mesh.uv_toggle_pin"},
        {"subdiv.uv_reset_flags","mesh.uv_reset_flags"},
        {"subdiv.diagnostics","mesh.diagnostics"},
        {"subdiv.boolean_union","mesh.boolean_union"},
        {"subdiv.boolean_difference","mesh.boolean_difference"},
        {"subdiv.boolean_reverse","mesh.boolean_reverse"},
        {"subdiv.boolean_intersection","mesh.boolean_intersection"},
        {"subdiv.hull_fit","mesh.hull_fit"},
        {"subdiv.hull_edit","mesh.hull_edit"},
        {"subdiv.hull_remove","mesh.hull_remove"},
        {"subdiv.preview_apply","polygon.preview_apply"},
        {"subdiv.preview_cancel","polygon.preview_cancel"},
        {"subdiv.delete","edit.delete"},
        {"subdiv.duplicate","edit.duplicate"}
    };return tools;
}
}
void NativeEditorController::refresh_subdivision_menu_state() {
    const auto primary=workspace_.selected_object();
    const auto* object=primary?workspace_.document().find_object(*primary):nullptr;
    const bool ready=workspace_.mode()==EditorMode::Edit && !playSession_.active() && !longJob_ &&
        !gizmoDragging_ && !voxel_authoring_active();
    const bool surface=ready && object && object->polygon && object->subdivision &&
        object->flags.visible && !object->flags.locked && !is_isolated_out(object->id);
    const auto enabled=[&](std::string_view id,bool value,std::string reason) {
        (void)workspace_.menus().set_enabled(id,value,value?std::string{}:std::move(reason));
    };
    for (auto id : {"subdiv.create_cube","subdiv.create_plane","create.subdivision"})
        enabled(id,ready && !polygonPreviewBefore_ && !mesh_authoring_dragging(),"Finish the active preview and stop Play before creating subdivision geometry.");
    enabled("subdiv.from_polygon",ready && object && object->polygon && !object->subdivision &&
        workspace_.selection_count()==1 && object->flags.visible && !object->flags.locked && !is_isolated_out(object->id),
        "Select one visible unlocked polygon mesh.");
    enabled("subdiv.from_voxels",ready && object && object->voxels && !object->polygon && !object->text3d && !object->gaborVolume &&
        workspace_.selection_count()==1 && object->flags.visible && !object->flags.locked && !is_isolated_out(object->id),
        "Select one visible unlocked voxel object.");
    for (auto id : {"subdiv.bake_polygon","subdiv.to_voxels"})
        enabled(id,surface && workspace_.selection_count()==1 && !polygonPreviewBefore_ && !mesh_authoring_dragging(),
            "Select one subdivision object and finish the active preview.");
    for (std::uint32_t level=0;level<=kMaxSubdivisionLevel;++level) {
        const auto id="subdiv.level_"+std::to_string(level);
        enabled(id,surface && !polygonPreviewBefore_ && !mesh_authoring_dragging(),"Select a subdivision surface and finish the preview.");
        (void)workspace_.menus().set_checked(id,object && object->subdivision && object->subdivision->level==level);
    }
    enabled("subdiv.level_up",surface && !polygonPreviewBefore_ && !mesh_authoring_dragging() &&
        object->subdivision->level<kMaxSubdivisionLevel,"Select a subdivision object below level 4.");
    enabled("subdiv.level_down",surface && !polygonPreviewBefore_ && !mesh_authoring_dragging() &&
        object->subdivision->level>0,"Select a subdivision object above level 0.");
    enabled("subdiv.sharp_toggle",surface && !polygonPreviewBefore_ && !mesh_authoring_dragging(),"Select a subdivision object and finish the preview.");
    enabled("subdiv.cage_toggle",surface,"Select a subdivision object.");
    (void)workspace_.menus().set_checked("subdiv.sharp_toggle",object && object->subdivision && object->subdivision->preserveSharpEdges);
    (void)workspace_.menus().set_checked("subdiv.cage_toggle",subdivisionCageVisible_);
    for (const auto& [id,destination] : subdivision_tools()) {
        const auto* target=std::as_const(workspace_.menus()).find(destination);
        const bool previewAction=id=="subdiv.preview_apply" || id=="subdiv.preview_cancel";
        enabled(id,surface && target && target->enabled && (!polygonPreviewBefore_ || previewAction),
            target && !target->enabled ? target->disabledReason : "Select a subdivision surface or its control components.");
        if (target && (id=="subdiv.vertex_mode" || id=="subdiv.edge_mode" || id=="subdiv.face_mode" || id=="subdiv.object_mode"))
            (void)workspace_.menus().set_checked(id,surface && (id=="subdiv.object_mode" ? selectionDomain_==SelectionDomain::Objects : target->checked));
    }
    enabled("subdiv.conversion_apply",voxel_authoring_preview_ready() && !longJob_,"Update the conversion preview first.");
    enabled("subdiv.conversion_cancel",voxel_authoring_active() && !longJob_,"No conversion preview is open.");
}
bool NativeEditorController::dispatch_subdivision_action(std::string_view action) {
    if (workspace_.mode()!=EditorMode::Edit || playSession_.active() || longJob_) return false;
    const auto primary=workspace_.selected_object();
    const auto* source=primary?workspace_.document().find_object(*primary):nullptr;
    if (action=="subdiv.from_voxels") return begin_geometry_conversion(false,true);
    if (action=="subdiv.to_voxels") return begin_geometry_conversion(true);
    if (action=="subdiv.create_cube" || action=="subdiv.create_plane" || action=="subdiv.from_polygon" ||
        action=="subdiv.bake_polygon") {
        const bool create=action=="subdiv.create_cube" || action=="subdiv.create_plane";
        const bool bake=action=="subdiv.bake_polygon";
        if (!create && (!editable(source) || is_isolated_out(source->id) || workspace_.selection_count()!=1 ||
            (bake ? !source->subdivision : source->subdivision.has_value()))) return false;
        cancel_mesh_authoring_drag();meshAuthoring_.reset();close_previews_for_voxel_edit("subdivision geometry");
        const auto before=capture_selection_snapshot();const auto id=workspace_.document().allocate_object_id();
        EditorObject object(id,create ? action=="subdiv.create_plane"?"Subdivision Plane":"Subdivision Cube" :
            source->name+(bake?" (baked polygon)":" (subdivision)"));
        object.voxelSizeMeters=0.1F;
        object.polygon=create ? action=="subdiv.create_plane"?make_editor_polygon_plane(id):make_editor_polygon_cube(id) :
            bake ? *editor_surface_mesh(*source) : *source->polygon;
        object.polygon->objectId=id;refresh_editor_polygon(*object.polygon);
        if (!bake) object.subdivision=SubdivisionSettings{};
        if (create) {object.transform.position=camera_.target;object.flags.anchored=action=="subdiv.create_plane";}
        else {
            object.transform=source->transform;object.voxelScale=source->voxelScale;object.voxelSizeMeters=source->voxelSizeMeters;
            object.flags=source->flags;object.flags.locked=false;object.parent=source->prefabLink?std::nullopt:source->parent;
            object.tags=source->tags;object.groups=source->groups;object.layer=source->layer;
            for (const auto& component : source->components)
                if (component.type=="dve.membership" || component.type=="dve.tags") object.components.push_back(component);
        }
        const auto result=workspace_.commands().execute(workspace_.document(),
            std::make_unique<AddObjectCommand>(std::move(object),bake?"Bake subdivision surface":"Create subdivision surface"));
        if (!result.success) {set_status(result.message,true);return false;}
        if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
        workspace_.select_object(id);selectionDomain_=SelectionDomain::Objects;polygonSelection_={};
        workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
        frame_selection();refresh_menu_state();
        set_status(bake?"Created a baked polygon; subdivision source retained":"Created subdivision surface; use the Subdivision menu for cage modes and modeling");
        return true;
    }
    if (!editable(source) || !source->subdivision || is_isolated_out(source->id)) {
        set_status("Select a visible unlocked subdivision surface first",true);return false;
    }
    if (action=="subdiv.cage_toggle") {
        subdivisionCageVisible_=!subdivisionCageVisible_;refresh_menu_state();return true;
    }
    if (action.starts_with("subdiv.level_") || action=="subdiv.sharp_toggle") {
        auto settings=*source->subdivision;
        if (action=="subdiv.level_up") settings.level=std::min(kMaxSubdivisionLevel,settings.level+1);
        else if (action=="subdiv.level_down") settings.level=settings.level>0?settings.level-1:0;
        else if (action=="subdiv.sharp_toggle") settings.preserveSharpEdges=!settings.preserveSharpEdges;
        else {
            const auto text=action.substr(std::string_view("subdiv.level_").size());
            if (text.size()!=1 || text[0]<'0' || text[0]>'4') return false;
            settings.level=std::uint32_t(text[0]-'0');
        }
        if (settings==*source->subdivision) return true;
        cancel_mesh_authoring_drag();meshAuthoring_.reset();close_previews_for_voxel_edit("subdivision settings");
        const auto before=capture_selection_snapshot();
        const auto result=workspace_.commands().execute(workspace_.document(),
            std::make_unique<SetSubdivisionSettingsCommand>(source->id,*source->subdivision,settings));
        if (result.success) workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
        set_status(result.success?"Subdivision level "+std::to_string(settings.level)+"; cage and materials retained":result.message,!result.success);
        refresh_menu_state();return result.success;
    }
    if (const auto at=subdivision_tools().find(action);at!=subdivision_tools().end()) return dispatch_action(at->second);
    return false;
}
void NativeEditorController::draw_subdivision_cage(const IEditorCanvas& canvas,UiRect viewport) const {
    if (workspace_.mode()!=EditorMode::Edit || playSession_.active()) return;
    const auto primary=workspace_.selected_object();const auto* object=primary?workspace_.document().find_object(*primary):nullptr;
    if (!object || !object->subdivision || !object->polygon || !object->flags.visible || is_isolated_out(object->id)) return;
    if (subdivisionCageDrawHash_!=object->polygon->contentHash || subdivisionCageDrawObject_!=object->id) {
        subdivisionCageDrawEdges_.clear();
        const auto allowed=subdivision_control_edges(*object->polygon);
        for (auto edge : polygon_edges(*object->polygon)) if (allowed.contains(edge.key())) subdivisionCageDrawEdges_.push_back(edge);
        subdivisionCageDrawHash_=object->polygon->contentHash;subdivisionCageDrawObject_=object->id;
    }
    if (subdivisionCageVisible_) {
        std::size_t drawn=0;
        for (const auto edge : subdivisionCageDrawEdges_) {
            if (++drawn>12000) break;
            const auto a=project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,object->polygon->vertices[edge.a].position));
            const auto b=project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,object->polygon->vertices[edge.b].position));
            if (a.depth<=camera_.nearPlane || b.depth<=camera_.nearPlane || !std::isfinite(a.x) || !std::isfinite(a.y) ||
                !std::isfinite(b.x) || !std::isfinite(b.y)) continue;
            const auto bounded=[](float v){return int(std::clamp(v,-100000.0F,100000.0F));};
            const bool selected=selectionDomain_==SelectionDomain::Polygons && polygonSelection_.mode==PolygonSelectionMode::Edge &&
                polygonSelection_.components.contains(edge.key());
            canvas.line(bounded(a.x),bounded(a.y),bounded(b.x),bounded(b.y),selected?0xffb75a:0x66d8e8,selected?2:1);
        }
        if (selectionDomain_==SelectionDomain::Polygons && polygonSelection_.mode==PolygonSelectionMode::Vertex)
            for (const auto id : polygon_all_components(*object->polygon,PolygonSelectionMode::Vertex)) {
                const auto p=project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,object->polygon->vertices[std::size_t(id)].position));
                if (p.visible) canvas.fill({int(p.x)-3,int(p.y)-3,7,7},polygonSelection_.components.contains(id)?0xffb75a:0x66d8e8);
            }
    }
    const UiRect caption{viewport.x+10,viewport.y+10,std::max(1,std::min(490,viewport.width-20)),42};
    RectClipCanvas painter(canvas,caption);painter.fill(caption,0x17202b);
    const std::string text="SUBDIVISION | level "+std::to_string(object->subdivision->level)+
        (selectionDomain_==SelectionDomain::Polygons?" | edit control cage":" | select object / cage modes");
    painter.text(caption.x+8,caption.y+16,elide_text_to_width(painter,text,caption.width-16),0x66d8e8);
    const std::string detail=object->subdivisionCache && !object->subdivisionCache->mesh ?
        object->subdivisionCache->error : "Extrude / inset / topology / UV / convert | Undo supported";
    painter.text(caption.x+8,caption.y+34,elide_text_to_width(painter,detail,caption.width-16),0xe0e9f5);
}

} // namespace dve::editor
