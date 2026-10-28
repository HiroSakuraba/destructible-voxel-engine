#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_exact_cad.hpp"
#include "dve/collision_hull.hpp"
#include "dve/voxel_metric.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <map>
namespace dve::editor {
struct NativeMeshAuthoringState {
    enum class Kind {None,Boolean,Topology,Uv,Hull,Displace};Kind kind{Kind::None};
    EditorObjectId object{},operand{};MeshBoolean boolean{MeshBoolean::Union};bool replace{};PolygonEditResult result;
    std::uint64_t operandStamp{};RigidTransform booleanPose{};Float3 booleanMetric{1,1,1};
    bool diagnostics{};EdgeBevelSettings bevel;
    PolygonSelection bevelSelection;
    PolygonFaceEditResult bevelAuthored;
    std::optional<ModelingTopology> bevelIdentities;
    std::map<ModelingId,std::set<ModelingId>> bevelDescendants;  // for named selection sets
    std::optional<std::string> bevelUvRecord;
    std::uint64_t bevelSourceHash{};
    std::optional<std::uint64_t> bevelSourceTopologyHash;
    std::optional<SubdivisionSettings> bevelSourceSubdivision;
    int bevelField{};bool bevelReplaceTyped{true},bevelInputValid{true};
    MeshDisplacementBakeSettings displace;PolygonDisplacementStats displaceStats{};
    std::uint64_t displaceSourceHash{};int displaceField{};bool displaceReplaceTyped{true},displaceInputValid{true};
    std::uint32_t channel{},uvMode{};std::set<std::uint32_t> corners;Float2 center{.5F,.5F};float zoom{1};bool uvFocus{true},texture{};
    float uvDensityTarget{512.0F};std::string uvDensityText;bool uvDensityTyping{};
    int tool{},axis{};bool dragging{},panning{},selecting{};int startX{},startY{},lastX{},lastY{};Float2 startUv{},pivot{},panStart{};
    PolygonUvAuthoring uvFlags;
    bool uvFlagsValid{},showUvSeams{},uvFlagsEnabled{true};
    EditorObjectId uvFlagsObject{};
    std::uint64_t uvFlagsMeshHash{};
    std::size_t uvFlagsComponentCount{};
    std::string uvFlagsData,uvFlagsError;
    std::vector<std::uint32_t> uvGeometric;
    std::string typed;std::optional<CookedPolygonAsset> uvBefore;
    SelectionOperation selectionOperation{SelectionOperation::Replace};
    std::set<std::uint32_t> hullSelection;std::optional<std::vector<Float3>> hullBefore;std::vector<Float3> hullPreview;bool hullValid{};
};
namespace {
EditorColor color(unsigned r,unsigned g,unsigned b){return (r<<16U)|(g<<8U)|b;}
UiRect bevel_panel(UiRect view) {return {view.x+8,view.y+8,std::max(1,std::min(420,view.width-16)),std::max(1,std::min(250,view.height-16))};}
UiRect bevel_button(UiRect view,int row,int column,int count=3) {
    const auto panel=bevel_panel(view);const int width=std::max(1,(panel.width-16)/count);
    return {panel.x+8+column*width,panel.y+28+row*24,width-3,21};
}
UiRect bevel_field(UiRect view,int field) {const auto p=bevel_panel(view);return {p.x+8,p.y+80+field*27,std::max(1,p.width-16),24};}
std::string bevel_value(const NativeMeshAuthoringState& state,int field) {
    if (field==1) return std::to_string(state.bevel.segments);
    return std::to_string(field==0?state.bevel.width:field==2?state.bevel.depth:state.bevel.smoothingAngle);
}
void update_bevel(NativeMeshAuthoringState& state,const EditorObject& object) {
    state.bevelInputValid=true;
    state.bevelIdentities.reset();state.bevelUvRecord.reset();
    const auto refuse=[&](std::string error){state.result={*object.polygon,state.bevelSelection,std::move(error)};};
    const auto sourceIds=object.modelingTopology?std::optional{modeling_topology_hash(*object.modelingTopology)}:std::nullopt;
    if(object.polygon->contentHash!=state.bevelSourceHash||object.subdivision!=state.bevelSourceSubdivision||sourceIds!=state.bevelSourceTopologyHash){refuse("Source geometry or topology changed; close and reopen the bevel preview");return;}
    if(object.prefabLink){refuse("Edit the prefab source or detach this instance before changing topology");return;}
    // Preview migration is local: Cancel never changes the source or its allocation floor.
    EditorObject migration(object.id,"Bevel identity snapshot");migration.polygon=object.polygon;migration.subdivision=object.subdivision;
    migration.modelingTopology=object.modelingTopology;migration.modelingNextId=object.modelingNextId;std::string error;
    if(!ensure_modeling_topology(migration,&error)){refuse(error);return;}
    if(!modeling_matches_legacy_partition(*object.polygon,*migration.modelingTopology)){refuse("Bevel does not support coincident independent vertices yet");return;}
    state.bevelAuthored=bevel_authored_editor_edges(*object.polygon,state.bevelSelection,state.bevel,object.voxelScale,object.subdivision?&*object.subdivision:nullptr);
    state.result=state.bevelAuthored.polygon;if(!state.result)return;
    const auto changed=advance_modeling_topology(*object.polygon,state.result.mesh,state.bevelAuthored.subdivision?&*state.bevelAuthored.subdivision:nullptr,
        *migration.modelingTopology,state.bevelAuthored.lineage,(std::max)(object.modelingNextId,migration.modelingTopology->nextId));
    if(!changed){refuse(changed.error);return;}
    const auto record=remap_polygon_uv_authoring_record(object,state.result.mesh,state.bevelAuthored.lineage,&error);
    if(!record){refuse(error);return;}
    state.bevelIdentities=changed.topology;state.bevelUvRecord=record;state.bevelDescendants=changed.descendants;
}
std::string displace_value(const NativeMeshAuthoringState& state,int field) {
    if (field==1) return std::to_string(state.displace.subdivisionLevels);
    return std::to_string(field==0?state.displace.strengthMeters:state.displace.midpoint);
}
void update_displace(NativeMeshAuthoringState& state,const EditorObject& object) {
    state.displaceInputValid=true;
    const auto refuse=[&](std::string error){state.result={*object.polygon,PolygonSelection{},std::move(error)};};
    if(object.polygon->contentHash!=state.displaceSourceHash){refuse("Source geometry changed; close and reopen the displacement preview");return;}
    if(object.prefabLink){refuse("Edit the prefab source or detach this instance before baking displacement");return;}
    auto baked=bake_editor_mesh_displacement(*object.polygon,state.displace);
    state.displaceStats=baked.stats;
    state.result=std::move(baked.polygon);
}
UiRect uv_panel(UiRect view){int width=std::max(1,view.width/2);return {view.x+view.width-width,view.y,width,view.height};}
UiRect uv_canvas(UiRect view){auto p=uv_panel(view);return {p.x+8,p.y+94,std::max(1,p.width-16),std::max(1,p.height-140)};}
Float2 texcoord(const PolygonVertex& v,unsigned channel){return channel?v.texcoord1:v.texcoord;}
ScreenPoint screen_uv(Float2 uv,const NativeMeshAuthoringState& s,UiRect view){auto r=uv_canvas(view);float scale=float(std::min(r.width,r.height))*.8F*s.zoom;return {float(r.x)+r.width*.5F+(uv.x-s.center.x)*scale,float(r.y)+r.height*.5F-(uv.y-s.center.y)*scale,0,true};}
Float2 from_screen(int x,int y,const NativeMeshAuthoringState& s,UiRect view){auto r=uv_canvas(view);float scale=std::max(1.0F,float(std::min(r.width,r.height))*.8F*s.zoom);return {s.center.x+(float(x)-r.x-r.width*.5F)/scale,s.center.y-(float(y)-r.y-r.height*.5F)/scale};}
float line_distance(float x,float y,ScreenPoint a,ScreenPoint b){float dx=b.x-a.x,dy=b.y-a.y;float t=std::clamp(((x-a.x)*dx+(y-a.y)*dy)/std::max(1e-10F,dx*dx+dy*dy),0.0F,1.0F);return std::hypot(x-a.x-t*dx,y-a.y-t*dy);}
bool triangle_contains(Float2 p,Float2 a,Float2 b,Float2 c){if(std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))<1e-12F)return false;auto cross=[](Float2 a,Float2 b,Float2 p){return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);};float x=cross(a,b,p),y=cross(b,c,p),z=cross(c,a,p);return (x>=0&&y>=0&&z>=0)||(x<=0&&y<=0&&z<=0);}
std::uint64_t stamp(const EditorDocument& d,EditorObjectId a,EditorObjectId b){std::uint64_t h=1469598103934665603ULL;auto mix=[&](std::uint64_t x){h^=x;h*=1099511628211ULL;};for(auto id:{a,b}){auto* o=d.find_object(id);if(!o){mix(0);continue;}mix(o->polygon?editor_surface_mesh(*o)->contentHash:0);mix(o->flags.locked);mix(o->flags.visible);auto p=d.world_transform(id).value_or(o->transform);for(float f:{p.position.x,p.position.y,p.position.z,p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w,o->voxelScale.x,o->voxelScale.y,o->voxelScale.z})mix(std::bit_cast<std::uint32_t>(f));}return h;}
std::vector<Float3> hull_points(const EditorObject& object){if(auto* c=find_component_by_type(std::span<const Component>(object.components),kCollisionHullComponent)){if(auto p=collision_hull_component_points(*c))return *p;}return {};}
std::unique_ptr<IEditorCommand> hull_command(EditorDocument& document,EditorObject& object,std::vector<Float3> points){
    auto text=encode_collision_hull_points(points);if(auto* c=find_component_by_type(std::span<Component>(object.components),kCollisionHullComponent))return std::make_unique<SetComponentPropertyCommand>(object.id,c->id,"points",c->properties.at("points"),ComponentValue{text},"Edit collision hull");
    Component c;c.id=document.allocate_component_id(object.id);c.type=std::string(kCollisionHullComponent);c.properties["points"]=text;return std::make_unique<AddComponentCommand>(object.id,std::move(c),"Create collision hull");
}

bool refresh_uv_flags(NativeMeshAuthoringState& state,const EditorObject& object) {
    const std::string* text=nullptr;
    std::size_t count=0;
    bool enabled=true;
    for(const auto& component : object.components) if(component.type==kUvAuthoringComponent) {
        ++count;
        enabled=component.enabled;
        const auto property=component.properties.find("state");
        if(property!=component.properties.end()) text=std::get_if<std::string>(&property->second);
    }
    const std::string_view data=text?std::string_view(*text):std::string_view{};
    if(state.uvFlagsObject==object.id && state.uvFlagsMeshHash==object.polygon->contentHash &&
       state.uvFlagsComponentCount==count && state.uvFlagsEnabled==enabled && data==state.uvFlagsData) return state.uvFlagsValid;
    state.uvFlagsObject=object.id;
    state.uvFlagsMeshHash=object.polygon->contentHash;
    state.uvFlagsComponentCount=count;
    state.uvFlagsEnabled=enabled;
    state.uvFlagsData=data;
    state.uvGeometric=polygon_geometric_vertices(*object.polygon);
    const auto flags=read_polygon_uv_authoring(object,&state.uvFlagsError);
    state.uvFlagsValid=bool(flags);
    state.uvFlags=flags.value_or(PolygonUvAuthoring{});
    return state.uvFlagsValid;
}
std::set<std::uint64_t> selected_uv_edges(const CookedPolygonAsset& mesh,
    const std::set<std::uint32_t>& corners) {
    const auto geometric=polygon_geometric_vertices(mesh);
    std::set<std::uint64_t> result;
    for(std::uint32_t c=0;c<mesh.indices.size();++c) {
        const auto other=c/3*3+(c+1)%3;
        if(!corners.contains(c) || !corners.contains(other)) continue;
        const auto a=geometric[mesh.indices[c]],b=geometric[mesh.indices[other]];
        if(a!=b)result.insert((std::uint64_t(std::min(a,b))<<32U)|std::max(a,b));
    }
    return result;
}

std::set<std::uint32_t> uv_click(const CookedPolygonAsset& m,const NativeMeshAuthoringState& s,UiRect view,int x,int y){
    std::set<std::uint32_t> hit;float best=10;std::optional<std::uint32_t> chosen;
    if(s.uvMode==0){for(std::uint32_t c=0;c<m.indices.size();++c){auto p=screen_uv(texcoord(m.vertices[m.indices[c]],s.channel),s,view);float distance=std::hypot(float(x)-p.x,float(y)-p.y);if(distance<best){best=distance;chosen=c;}}
        if(chosen){auto index=m.indices[*chosen];for(std::uint32_t c=0;c<m.indices.size();++c)if(m.indices[c]==index)hit.insert(c);}}
    else if(s.uvMode==1){for(std::uint32_t c=0;c<m.indices.size();++c){auto other=c/3*3+(c+1)%3;auto a=screen_uv(texcoord(m.vertices[m.indices[c]],s.channel),s,view),b=screen_uv(texcoord(m.vertices[m.indices[other]],s.channel),s,view);auto distance=line_distance(float(x),float(y),a,b);if(distance<best){best=distance;chosen=c;}}
        if(chosen){auto other=*chosen/3*3+(*chosen+1)%3;auto a=m.indices[*chosen],b=m.indices[other];for(std::uint32_t c=0;c<m.indices.size();++c)if(m.indices[c]==a||m.indices[c]==b)hit.insert(c);}}
    else{auto p=from_screen(x,y,s,view);for(std::uint32_t f=0;f<m.indices.size()/3;++f){if(!triangle_contains(p,texcoord(m.vertices[m.indices[f*3]],s.channel),texcoord(m.vertices[m.indices[f*3+1]],s.channel),texcoord(m.vertices[m.indices[f*3+2]],s.channel)))continue;
            if(s.uvMode==2){hit={f*3,f*3+1,f*3+2};}else for(const auto& island:polygon_uv_islands(m,s.channel,s.uvFlags.enabled?s.uvFlags.seams[s.channel]:std::set<std::uint64_t>{}))if(island.contains(f))for(auto face:island)for(unsigned j=0;j<3;++j)hit.insert(face*3+j);break;}}
    return hit;
}
}
std::set<EditorObjectId> NativeEditorController::mesh_authoring_hidden_objects() const{

    if(!meshAuthoring_||!meshAuthoring_->result)return {};const auto& s=*meshAuthoring_;
    if(s.kind==NativeMeshAuthoringState::Kind::Boolean)return {s.object,s.operand};if(s.kind==NativeMeshAuthoringState::Kind::Topology && s.result)return {s.object};if(s.kind==NativeMeshAuthoringState::Kind::Displace && s.result)return {s.object};return {};
}
void NativeEditorController::refresh_mesh_authoring_menu_state(){
    auto primary=workspace_.selected_object();auto* object=primary?workspace_.document().find_object(*primary):nullptr;
    bool mesh=object&&object->polygon&&object->flags.visible&&!object->flags.locked&&!is_isolated_out(object->id)&&workspace_.mode()==EditorMode::Edit&&!playSession_.active()&&!longJob_;
    auto enabled=[&](std::string_view id,bool value,std::string_view reason){(void)workspace_.menus().set_enabled(id,value,value?std::string{}:std::string(reason));};
    bool pair=mesh&&workspace_.selection_count()==2;if(pair)for(auto id:workspace_.selected_objects()){auto* o=workspace_.document().find_object(id);pair=pair&&o&&o->polygon&&!o->flags.locked&&o->flags.visible&&!is_isolated_out(id);}
    for(auto id:{"mesh.boolean_union","mesh.boolean_difference","mesh.boolean_reverse","mesh.boolean_intersection"})enabled(id,pair&&mesh_boolean_available(),mesh_boolean_available()?"Select exactly two visible unlocked polygon meshes":"This build needs the Manifold authoring backend");
    {
        bool exact=pair;if(exact)for(auto id:workspace_.selected_objects()){auto* o=workspace_.document().find_object(id);exact=exact&&o&&exact_brep_source(*o).has_value();}
        for(auto id:{"mesh.exact_boolean_union","mesh.exact_boolean_difference","mesh.exact_boolean_reverse","mesh.exact_boolean_intersection"})
            enabled(id,exact,pair?"Both operands need an exact CAD source (Polygon > Import Exact CAD)":"Select exactly two visible unlocked polygon meshes");
        enabled("mesh.exact_collision",mesh&&object&&exact_brep_source(*object).has_value(),
                "Select a visible unlocked polygon object with an exact CAD source");
        for(auto id:{"mesh.exact_cope","mesh.exact_cope_solid","mesh.exact_miter","mesh.exact_resize_tube","mesh.exact_edge_fillet","mesh.exact_variable_fillet","mesh.exact_face_fillet"}){const std::string reason=exact_tube_disabled_reason(id);enabled(id,reason.empty(),reason);}
    }
    for(auto id:{"mesh.hull_fit","mesh.hull_edit"})enabled(id,mesh&&mesh_boolean_available(),"Select an unlocked polygon mesh in a build with Manifold authoring");
    enabled("mesh.hull_remove",mesh&&object&&find_component_by_type(std::span<const Component>(object->components),kCollisionHullComponent),"Select a polygon with an authored collision hull");
    for(auto id:{"mesh.uv_editor","mesh.diagnostics"})enabled(id,mesh,"Select an unlocked polygon mesh in Edit mode");
    enabled("mesh.displace",mesh,"Select an unlocked polygon mesh in Edit mode");
    const bool uvWorkspace=mesh && meshAuthoring_ && meshAuthoring_->kind==NativeMeshAuthoringState::Kind::Uv &&
        meshAuthoring_->object==object->id;
    const bool flagPreview=polygonPreviewBefore_ || mesh_authoring_dragging() || gizmoDragging_ ||
        (meshAuthoring_ && meshAuthoring_->kind!=NativeMeshAuthoringState::Kind::None &&
         meshAuthoring_->kind!=NativeMeshAuthoringState::Kind::Uv);
    const bool flagsValid=mesh && (meshAuthoring_ ? refresh_uv_flags(*meshAuthoring_,*object)
                                                   : bool(read_polygon_uv_authoring(*object)));
    const bool edgesSelected=mesh && ((selectionDomain_==SelectionDomain::Polygons &&
        polygonSelection_.object==object->id && polygonSelection_.mode==PolygonSelectionMode::Edge &&
        !polygonSelection_.components.empty()) || (uvWorkspace && meshAuthoring_->uvMode==1 &&
        !meshAuthoring_->corners.empty()));
    for(auto id:{"mesh.uv_mark_seam","mesh.uv_clear_seam"})
        enabled(id,edgesSelected && flagsValid && !flagPreview,"Select edges with valid saved UV flags; apply/close previews first");
    const bool facePins=uvWorkspace && polygonSelection_.object==object->id &&
        polygonSelection_.mode==PolygonSelectionMode::Face && !polygonSelection_.components.empty();
    enabled("mesh.uv_toggle_pin",uvWorkspace && flagsValid && !flagPreview &&
        (!meshAuthoring_->corners.empty() || facePins),"Select UV corners and apply/close previews first");
    enabled("mesh.uv_reset_flags",mesh && !flagPreview &&
        find_component_by_type(std::span<const Component>(object->components),kUvAuthoringComponent),
        "Select a polygon with saved UV flags; apply/close previews first");
    bool components=mesh&&selectionDomain_==SelectionDomain::Polygons&&!polygonSelection_.components.empty();
    for(auto id:{"mesh.subdivide","mesh.hard_normals","mesh.smooth_normals"})enabled(id,components,"Select polygon components first");
    for(auto id:{"mesh.split_edges","mesh.flip_diagonal"})enabled(id,components&&polygonSelection_.mode==PolygonSelectionMode::Edge,"Select polygon edges first");
    {
        // Mirror begin_polygon_boundary's up-front refusals so the menu never offers a preview that cannot open.
        const bool edges=components&&polygonSelection_.mode==PolygonSelectionMode::Edge&&polygonSelection_.object==object->id;
        const bool meshPreview=meshAuthoring_&&(meshAuthoring_->kind==NativeMeshAuthoringState::Kind::Topology||meshAuthoring_->kind==NativeMeshAuthoringState::Kind::Boolean);
        const bool busy=polygonPreviewBefore_.has_value()||meshPreview||gizmoDragging_||mesh_authoring_dragging()||voxel_authoring_active();
        const bool prefab=object&&object->prefabLink.has_value();
        const std::string_view reason=!edges?"Select complete boundary loop edges first":
            workspace_.selection_count()!=1?"Select exactly one polygon mesh":
            prefab?"Edit the prefab source or detach this instance before changing topology":"Apply or close the active preview first";
        for(auto id:{"mesh.bridge","mesh.cap"})enabled(id,edges&&workspace_.selection_count()==1&&!prefab&&!busy,reason);
    }
    {
        // Mirror the bevel dispatch refusals so the menu never offers a preview that cannot open.
        const bool edges=components&&polygonSelection_.mode==PolygonSelectionMode::Edge&&polygonSelection_.object==object->id;
        const bool single=workspace_.selection_count()==1;const bool prefab=object&&object->prefabLink.has_value();
        const std::string_view reason=!edges?"Select crease or open boundary edges in Edge mode first":
            !single?"Select exactly one polygon mesh":"Edit the prefab source or detach this instance before changing topology";
        for(auto id:{"mesh.bevel_edges","mesh.chamfer_edges","mesh.fillet_edges"})enabled(id,edges&&single&&!prefab,reason);
    }
}
bool NativeEditorController::mesh_authoring_preview_open() const noexcept {return meshAuthoring_&&meshAuthoring_->kind!=NativeMeshAuthoringState::Kind::None;}
bool NativeEditorController::mesh_authoring_dragging() const noexcept {return meshAuthoring_&&(meshAuthoring_->uvBefore||meshAuthoring_->hullBefore);}
void NativeEditorController::cancel_mesh_authoring_drag(){if(!meshAuthoring_)return;auto& s=*meshAuthoring_;if(s.uvBefore){if(auto* o=workspace_.document().find_object(s.object))o->polygon=*s.uvBefore;}s.uvBefore.reset();s.hullBefore.reset();s.hullPreview.clear();s.typed.clear();s.dragging=s.panning=s.selecting=false;}

bool NativeEditorController::dispatch_mesh_authoring_action(std::string_view action){
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active()||longJob_){set_status("Mesh authoring requires Edit mode",true);return false;}
    if(!meshAuthoring_)meshAuthoring_=std::make_shared<NativeMeshAuthoringState>();auto& s=*meshAuthoring_;
    const auto primary=workspace_.selected_object();auto* object=primary?workspace_.document().find_object(*primary):nullptr;
    if(!object||!object->polygon||object->flags.locked||!object->flags.visible||is_isolated_out(object->id)){set_status("Select a visible unlocked polygon mesh first",true);return false;}

    if(action=="mesh.uv_mark_seam" || action=="mesh.uv_clear_seam" ||
       action=="mesh.uv_toggle_pin" || action=="mesh.uv_reset_flags") {
        if(polygonPreviewBefore_ || mesh_authoring_dragging() || gizmoDragging_ ||
           (s.kind!=NativeMeshAuthoringState::Kind::None && s.kind!=NativeMeshAuthoringState::Kind::Uv)) {
            set_status("Apply or close the active mesh preview before editing UV flags",true);
            return false;
        }
        if(action=="mesh.uv_reset_flags") {
            auto command=std::make_unique<CompoundCommand>("Reset UV seams and pins");
            bool found=false;
            for(const auto& component : object->components) if(component.type==kUvAuthoringComponent) {
                command->add(std::make_unique<RemoveComponentCommand>(object->id,component.id,"Remove UV flags"));
                found=true;
            }
            if(!found) { set_status("No saved UV flags to reset",true); return false; }
            const auto snapshot=capture_selection_snapshot();
            const auto result=workspace_.commands().execute(workspace_.document(),std::move(command));
            if(result.success)workspace_.commands().set_last_selection_transition(snapshot,capture_selection_snapshot());
            s.uvFlagsObject=0;
            if(s.object==object->id)(void)refresh_uv_flags(s,*object);
            set_status(result.success?"Reset saved UV seams and pins":result.message,!result.success);
            return result.success;
        }
        if(!refresh_uv_flags(s,*object)) { set_status(s.uvFlagsError,true); return false; }
        const unsigned channel=s.kind==NativeMeshAuthoringState::Kind::Uv && s.object==object->id?s.channel:0;
        auto flags=s.uvFlags;
        flags.enabled=true;
        if(action=="mesh.uv_toggle_pin") {
            if(s.kind!=NativeMeshAuthoringState::Kind::Uv || s.object!=object->id) {
                set_status("Select UV corners in the active UV workspace",true);
                return false;
            }
            auto pinCorners=s.corners;
            if(!s.uvFocus && polygonSelection_.object==object->id && polygonSelection_.mode==PolygonSelectionMode::Face) {
                pinCorners.clear();
                for(const auto face : polygonSelection_.components) for(unsigned j=0;j<3;++j)
                    if(face<object->polygon->indices.size()/3)pinCorners.insert(std::uint32_t(face*3+j));
            }
            if(pinCorners.empty()) { set_status("Select UV corners or faces to pin",true); return false; }
            for(const auto corner : pinCorners) {
                if(corner>=object->polygon->indices.size())continue;
                if(!flags.pins[channel].erase(corner))flags.pins[channel].insert(corner);
            }
        } else {
            std::set<std::uint64_t> edges;
            const bool uvEdges=s.kind==NativeMeshAuthoringState::Kind::Uv && s.object==object->id && s.uvMode==1;
            if(uvEdges && s.uvFocus)edges=selected_uv_edges(*object->polygon,s.corners);
            else if(selectionDomain_==SelectionDomain::Polygons && polygonSelection_.object==object->id &&
                    polygonSelection_.mode==PolygonSelectionMode::Edge)
                edges=polygonSelection_.components;
            else if(uvEdges)edges=selected_uv_edges(*object->polygon,s.corners);
            if(edges.empty()) { set_status("Select geometric edges or UV edges first",true); return false; }
            for(const auto edge : edges) {
                if(action=="mesh.uv_mark_seam")flags.seams[channel].insert(edge);
                else flags.seams[channel].erase(edge);
            }
        }
        if(flags.enabled==s.uvFlags.enabled && flags.seams==s.uvFlags.seams && flags.pins==s.uvFlags.pins) {
            set_status("Saved UV flags already match the selection");
            return true;
        }
        std::string error;
        auto command=make_polygon_uv_authoring_command(workspace_.document(),*object,flags,&error);
        if(!command) { set_status(error,true); return false; }
        const auto snapshot=capture_selection_snapshot();
        const auto result=workspace_.commands().execute(workspace_.document(),std::move(command));
        if(result.success)workspace_.commands().set_last_selection_transition(snapshot,capture_selection_snapshot());
        s.uvFlagsObject=0;
        if(result.success) {
            s.object=object->id;
            s.channel=channel;
            s.showUvSeams=true;
            (void)refresh_uv_flags(s,*object);
        }
        set_status(result.success?"Saved UV seams and pins for channel "+std::to_string(channel):result.message,!result.success);
        return result.success;
    }

    if(action=="mesh.exact_collision"){cancel_mesh_authoring_drag();return start_exact_collision(object->id);}
    if(action=="mesh.exact_cope"||action=="mesh.exact_cope_solid"||action=="mesh.exact_miter"||action=="mesh.exact_resize_tube"||action=="mesh.exact_edge_fillet"||action=="mesh.exact_variable_fillet"||action=="mesh.exact_face_fillet"){cancel_mesh_authoring_drag();return dispatch_exact_tube_action(action);}
    if(action.starts_with("mesh.exact_boolean_")){
        cancel_mesh_authoring_drag();close_previews_for_voxel_edit("exact Boolean");
        if(workspace_.selection_count()!=2){set_status("Select exactly two polygon operands with exact CAD sources; the active object is A",true);return false;}
        auto other=*std::find_if(workspace_.selected_objects().begin(),workspace_.selected_objects().end(),[&](auto id){return id!=object->id;});
        if(is_isolated_out(other)){set_status("Boolean operand is isolated out",true);return false;}
        const auto op=action=="mesh.exact_boolean_difference"?MeshBoolean::Difference:action=="mesh.exact_boolean_reverse"?MeshBoolean::ReverseDifference:
            action=="mesh.exact_boolean_intersection"?MeshBoolean::Intersection:MeshBoolean::Union;
        return start_exact_boolean(object->id,other,op);
    }
    if(action.starts_with("mesh.boolean_")){
        cancel_mesh_authoring_drag();close_previews_for_voxel_edit("mesh Boolean");
        if(workspace_.selection_count()!=2){set_status("Select exactly two owned polygon operands; active object is A",true);return false;}
        auto other=*std::find_if(workspace_.selected_objects().begin(),workspace_.selected_objects().end(),[&](auto id){return id!=object->id;});
        if(is_isolated_out(other)){set_status("Boolean operand is isolated out",true);return false;}
        auto op=action=="mesh.boolean_difference"?MeshBoolean::Difference:action=="mesh.boolean_reverse"?MeshBoolean::ReverseDifference:action=="mesh.boolean_intersection"?MeshBoolean::Intersection:MeshBoolean::Union;
        auto result=boolean_editor_polygons(workspace_.document(),object->id,other,op);if(!result){set_status(result.error,true);return false;}
        s.kind=NativeMeshAuthoringState::Kind::Boolean;s.object=object->id;s.operand=other;s.boolean=op;s.result=std::move(result);s.replace=false;
        s.operandStamp=stamp(workspace_.document(),s.object,s.operand);s.booleanPose=workspace_.document().world_transform(s.object).value_or(object->transform);s.booleanMetric=object->voxelScale;
        set_status("Mesh Boolean preview: U/D/X/I operation, Tab swaps operands, R new/replace A, Enter apply, Esc cancel");return true;
    }
    if(action=="mesh.bevel_edges" || action=="mesh.chamfer_edges" || action=="mesh.fillet_edges"){
        cancel_mesh_authoring_drag();close_previews_for_voxel_edit("edge bevel");
        if(workspace_.selection_count()!=1||object->prefabLink||selectionDomain_!=SelectionDomain::Polygons || polygonSelection_.object!=object->id || polygonSelection_.mode!=PolygonSelectionMode::Edge){set_status("Select crease or open boundary edges on one owned mesh in Edge mode first",true);return false;}
        s.bevel=EdgeBevelSettings{};s.bevelSelection=polygonSelection_;
        s.bevelSourceHash=object->polygon->contentHash;s.bevelSourceSubdivision=object->subdivision;
        s.bevelSourceTopologyHash=object->modelingTopology?std::optional{modeling_topology_hash(*object->modelingTopology)}:std::nullopt;
        if(action=="mesh.chamfer_edges") {s.bevel.depth=0;s.bevel.segments=1;}
        if(action=="mesh.fillet_edges") {s.bevel.depth=1;s.bevel.segments=4;s.bevel.smoothingAngle=60;}
        update_bevel(s,*object);if(!s.result){set_status(s.result.error,true);return false;}
        s.kind=NativeMeshAuthoringState::Kind::Topology;s.object=object->id;s.bevelField=0;s.bevelReplaceTyped=true;s.typed.clear();s.operandStamp=stamp(workspace_.document(),s.object,s.object);
        set_status("Edge bevel options: Width, Segments, Depth and Smoothing Angle; Enter Apply, Esc Cancel");return true;
    }
    if(action=="mesh.uv_editor"){
        cancel_mesh_authoring_drag();if(s.kind==NativeMeshAuthoringState::Kind::Uv&&s.object==object->id){s.kind=NativeMeshAuthoringState::Kind::None;return true;}
        s.uvFlagsObject=0;
        close_previews_for_voxel_edit("UV Workspace");if(!set_polygon_mode(PolygonSelectionMode::Face))return false;
        s.kind=NativeMeshAuthoringState::Kind::Uv;s.object=object->id;s.channel=0;s.center={.5F,.5F};s.zoom=1;s.tool=0;s.uvFocus=true;s.corners.clear();
        s.showUvSeams=true;
        if(!refresh_uv_flags(s,*object))set_status(s.uvFlagsError,true);
        else set_status("UV Workspace: P saved pins; M mark seam, Shift+M clear; U unwrap, K pack; Tab channel");
        return true;
    }
    if(action=="mesh.hull_fit"||action=="mesh.hull_edit"){
        cancel_mesh_authoring_drag();close_previews_for_voxel_edit("hull authoring");
        auto points=hull_points(*object);
        if(action=="mesh.hull_fit"||points.empty()){
            std::set<std::tuple<float,float,float>> seen;points.clear();for(const auto& v:object->polygon->vertices)if(seen.emplace(v.position.x,v.position.y,v.position.z).second)points.push_back(v.position);
            auto hull=convex_editor_hull(object->id,points);if(!hull){set_status(hull.error,true);return false;}points.clear();for(const auto& v:hull.mesh.vertices)points.push_back(v.position);
            auto result=workspace_.commands().execute(workspace_.document(),hull_command(workspace_.document(),*object,points));if(!result.success){set_status(result.message,true);return false;}
        }
        s.kind=NativeMeshAuthoringState::Kind::Hull;s.object=object->id;s.hullSelection={0};s.tool=0;s.axis=0;set_selection_domain(SelectionDomain::Objects);
        set_status("Hull points are collision-only: click/Shift select; A all; X/Y/Z axis; G/R/S then type; N add, Delete remove, B simplify to bounds, Esc close");return true;
    }
    if(action=="mesh.hull_remove"){
        cancel_mesh_authoring_drag();auto* c=find_component_by_type(std::span<Component>(object->components),kCollisionHullComponent);if(!c){set_status("No authored hull on this object",true);return false;}
        auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<RemoveComponentCommand>(object->id,c->id,"Remove collision hull"));s.kind=NativeMeshAuthoringState::Kind::None;set_status(result.success?"Removed authored hull; default collision proxy restored":result.message,!result.success);return result.success;
    }
    if(action=="mesh.diagnostics"){s.object=object->id;s.diagnostics=!s.diagnostics;auto d=diagnose_polygon_topology(*object->polygon);set_status("Topology: "+std::to_string(d.boundaries)+" boundary edges, "+std::to_string(d.nonmanifold)+" nonmanifold, "+std::to_string(d.inconsistentWinding)+" winding errors, "+std::to_string(d.degenerate)+" degenerate triangles");return true;}
    if(action=="mesh.displace"){
        if(s.kind==NativeMeshAuthoringState::Kind::Topology||s.kind==NativeMeshAuthoringState::Kind::Boolean) {
            set_status("Apply or close the active mesh preview first",true);return false;
        }
        cancel_mesh_authoring_drag();close_previews_for_voxel_edit("mesh displacement");
        s.displace=MeshDisplacementBakeSettings{};s.displaceSourceHash=object->polygon->contentHash;
        s.kind=NativeMeshAuthoringState::Kind::Displace;s.object=object->id;s.displaceField=0;s.displaceReplaceTyped=true;s.typed.clear();
        update_displace(s,*object);
        set_status(s.result?"Displacement options: Strength (m), Subdivisions, Midpoint; Enter Apply, Esc Cancel":s.result.error,!s.result);return true;
    }
    TopologyEdit op;
    if(action=="mesh.bridge"||action=="mesh.cap") {
        if(s.kind==NativeMeshAuthoringState::Kind::Topology||s.kind==NativeMeshAuthoringState::Kind::Boolean) {
            set_status("Apply or close the active mesh preview first",true);return false;
        }
        const auto opened=begin_polygon_boundary(action=="mesh.bridge");
        if(opened){s.kind=NativeMeshAuthoringState::Kind::None;s.corners.clear();}
        return opened;
    }
    if(action=="mesh.subdivide")op=TopologyEdit::Subdivide;else if(action=="mesh.split_edges")op=TopologyEdit::SplitEdges;else if(action=="mesh.flip_diagonal")op=TopologyEdit::FlipDiagonal;else if(action=="mesh.hard_normals")op=TopologyEdit::HardNormals;else if(action=="mesh.smooth_normals")op=TopologyEdit::SmoothNormals;else return false;
    cancel_mesh_authoring_drag();if(selectionDomain_!=SelectionDomain::Polygons||polygonSelection_.object!=object->id){set_status("Enter polygon component mode and select components first",true);return false;}
    auto edited=edit_polygon_topology(*object->polygon,polygonSelection_,op);if(!edited){set_status(edited.error,true);return false;}
    const auto before=capture_selection_snapshot();auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<ReplacePolygonCommand>(object->id,*object->polygon,std::move(edited.mesh),"Edit polygon topology"));
    if(result.success){polygonSelection_=edited.selection;workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());if(s.kind==NativeMeshAuthoringState::Kind::Uv)s.corners.clear();}set_status(result.success?"Edited polygon topology":result.message,!result.success);return result.success;
}

// Opens the (uncertified) Manifold Boolean preview for a, b without applying it; used as the
// fallback when brepkernel refuses an exact Boolean.
bool NativeEditorController::open_mesh_boolean_preview(EditorObjectId a,EditorObjectId b,MeshBoolean op){
    if(!mesh_boolean_available())return false;
    auto& document=workspace_.document();const auto* objectA=document.find_object(a);if(!objectA)return false;
    auto preview=boolean_editor_polygons(document,a,b,op);if(!preview)return false;
    if(!meshAuthoring_)meshAuthoring_=std::make_shared<NativeMeshAuthoringState>();auto& s=*meshAuthoring_;
    s.kind=NativeMeshAuthoringState::Kind::Boolean;s.object=a;s.operand=b;s.boolean=op;s.result=std::move(preview);s.replace=false;
    s.operandStamp=stamp(document,a,b);s.booleanPose=document.world_transform(a).value_or(objectA->transform);s.booleanMetric=objectA->voxelScale;
    return true;
}

bool NativeEditorController::mesh_authoring_key(std::string_view key,bool control,bool shift,bool alt){
    if(polygonPreviewBefore_)return false;
    if(!meshAuthoring_||workspace_.mode()!=EditorMode::Edit||playSession_.active()||alt)return false;auto& s=*meshAuthoring_;auto* object=workspace_.document().find_object(s.object);
    if(!object||!object->polygon||object->flags.locked||!object->flags.visible||is_isolated_out(s.object)){cancel_mesh_authoring_drag();s.kind=NativeMeshAuthoringState::Kind::None;return false;}
    if(s.kind==NativeMeshAuthoringState::Kind::Topology){
        if(key=="escape"){s.kind=NativeMeshAuthoringState::Kind::None;refresh_menu_state();set_status("Bevel cancelled; source unchanged");return true;}
        if(key=="enter"||key=="return"){
            if(workspace_.selection_count()!=1||workspace_.selected_object()!=s.object){set_status("Select the bevel source before applying its preview",true);return true;}
            if(selectionDomain_!=SelectionDomain::Polygons||polygonSelection_.object!=s.object||polygonSelection_.mode!=PolygonSelectionMode::Edge||polygonSelection_.components!=s.bevelSelection.components){set_status("Return to the preview's original edge selection or close and reopen the bevel",true);return true;}
            const auto topologyHash=object->modelingTopology?std::optional{modeling_topology_hash(*object->modelingTopology)}:std::nullopt;
            if(object->polygon->contentHash!=s.bevelSourceHash||object->subdivision!=s.bevelSourceSubdivision||topologyHash!=s.bevelSourceTopologyHash){update_bevel(s,*object);set_status(s.result.error,true);return true;}
            if(!s.bevelInputValid){set_status(s.result.error,true);return true;}
            if(stamp(workspace_.document(),s.object,s.object)!=s.operandStamp){update_bevel(s,*object);s.operandStamp=stamp(workspace_.document(),s.object,s.object);set_status("Geometry changed: inspect updated bevel and press Enter again",!s.result);return true;}
            // Recheck saved flags and settings at Apply, including inspector changes since the preview.
            update_bevel(s,*object);
            if(!s.result||!s.bevelIdentities){set_status(s.result.error,true);return true;}
            note_selection_lineage(object->id,s.bevelDescendants);
            auto command=std::make_unique<CompoundCommand>("Bevel / chamfer / rounded fillet");
            command->add(std::make_unique<ReplacePolygonCommand>(object->id,*object->polygon,s.result.mesh,"Bevel authored edges",0,s.bevelAuthored.subdivision,false,s.bevelIdentities));
            if(s.bevelUvRecord&&!s.bevelUvRecord->empty()) {
                const auto* component=find_component_by_type(std::span<const Component>(object->components),kUvAuthoringComponent);
                if(!component){set_status("Saved UV flags changed; recompute the bevel preview",true);return true;}
                const auto at=component->properties.find("state");
                command->add(std::make_unique<SetComponentPropertyCommand>(object->id,component->id,"state",at==component->properties.end()?std::optional<ComponentValue>{}:at->second,ComponentValue{*s.bevelUvRecord},"Preserve bevel UV seams and pins"));
            }
            auto before=capture_selection_snapshot();auto result=workspace_.commands().execute(workspace_.document(),std::move(command));
            if(result.success){polygonSelection_=s.result.selection;workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());s.kind=NativeMeshAuthoringState::Kind::None;}
            set_status(result.success?"Applied edge bevel; Undo restores cage and selection":result.message,!result.success);refresh_menu_state();return true;
        }
        if (control) return false;
        bool update=false;
        if(key=="tab") {s.bevelField=(s.bevelField+(shift?3:1))%4;s.typed.clear();s.bevelReplaceTyped=true;update=true;}
        else if(key=="f") {s.bevel.offsetType=s.bevel.offsetType==BevelOffsetType::Fractional?BevelOffsetType::Absolute:BevelOffsetType::Fractional;if(s.bevel.offsetType==BevelOffsetType::Fractional)s.bevel.width=std::min(1.0F,s.bevel.width);update=true;}
        else if(key=="w") {if(s.bevel.offsetType==BevelOffsetType::Absolute){s.bevel.worldSpace=!s.bevel.worldSpace;update=true;}else return true;}
        else if(key=="a") {s.bevel.autoFit=!s.bevel.autoFit;update=true;}
        else if(key=="c") {s.bevel.segments=1;s.bevel.depth=0;s.bevel.smoothingAngle=30;update=true;}
        else if(key=="r") {s.bevel.segments=4;s.bevel.depth=1;s.bevel.smoothingAngle=60;update=true;}
        else if(key=="up"||key=="down"||key=="left"||key=="right") {
            const float direction=key=="up"||key=="right"?1.0F:-1.0F;
            if(s.bevelField==1)s.bevel.segments=std::uint32_t(std::clamp(int(s.bevel.segments)+int(direction),1,16));
            else if(s.bevelField==2)s.bevel.depth=std::clamp(s.bevel.depth+direction*.1F,0.0F,1.0F);
            else if(s.bevelField==3)s.bevel.smoothingAngle=std::clamp(s.bevel.smoothingAngle+direction*5,0.0F,180.0F);
            else s.bevel.width=std::clamp(s.bevel.width*(direction>0?1.1F:1/1.1F),0.000001F,s.bevel.offsetType==BevelOffsetType::Fractional?1.0F:1000000.0F);
            update=true;
        }
        if(update) {s.typed.clear();s.bevelReplaceTyped=true;update_bevel(s,*object);return true;}
        if(key=="backspace") {if(s.bevelReplaceTyped){s.typed.clear();s.bevelReplaceTyped=false;}else if(!s.typed.empty())s.typed.pop_back();}
        else if((key.size()==1&&((key[0]>='0'&&key[0]<='9')||key[0]=='.'))||key=="period") {
            if(s.bevelReplaceTyped){s.typed.clear();s.bevelReplaceTyped=false;}
            if(s.typed.size()<16)s.typed+=key=="period"?".":std::string(key);
        } else return true;
        char* end{};const float value=std::strtof(s.typed.c_str(),&end);
        if(s.typed.empty()||end!=s.typed.c_str()+s.typed.size()||!std::isfinite(value)) {s.bevelInputValid=false;s.result.error="Enter a finite number for the highlighted option";return true;}
        const bool valid=s.bevelField==1?value>=1 && value<=16 && std::floor(value)==value:
            s.bevelField==2?value>=0 && value<=1:s.bevelField==3?value>=0 && value<=180:
            value>0 && (s.bevel.offsetType!=BevelOffsetType::Fractional || value<=1);
        if(!valid){s.bevelInputValid=false;s.result.error="Width >0 (fraction <=1), Segments 1..16, Depth 0..1, Smoothing 0..180";return true;}
        if(s.bevelField==0)s.bevel.width=value;else if(s.bevelField==1)s.bevel.segments=std::uint32_t(value);
        else if(s.bevelField==2)s.bevel.depth=value;else s.bevel.smoothingAngle=value;
        update_bevel(s,*object);return true;
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Displace){
        if(control)return false;
        if(key=="escape"){s.kind=NativeMeshAuthoringState::Kind::None;refresh_menu_state();set_status("Displacement cancelled; source unchanged");return true;}
        if(key=="enter"||key=="return"){
            if(workspace_.selection_count()!=1||workspace_.selected_object()!=s.object){set_status("Select the displacement source before applying its preview",true);return true;}
            if(object->polygon->contentHash!=s.displaceSourceHash){update_displace(s,*object);set_status(s.result.error,true);return true;}
            if(!s.displaceInputValid){set_status(s.result.error,true);return true;}
            update_displace(s,*object);
            if(!s.result){set_status(s.result.error,true);return true;}
            const auto before=capture_selection_snapshot();
            auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<ReplacePolygonCommand>(object->id,*object->polygon,s.result.mesh,"Displace from height texture"));
            if(result.success){workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());s.kind=NativeMeshAuthoringState::Kind::None;}
            set_status(result.success?"Baked height displacement into the mesh; Undo restores the original":result.message,!result.success);refresh_menu_state();return true;
        }
        bool update=false;
        if(key=="tab") {s.displaceField=(s.displaceField+(shift?2:1))%3;s.typed.clear();s.displaceReplaceTyped=true;update=true;}
        else if(key=="up"||key=="down"||key=="left"||key=="right") {
            const float direction=key=="up"||key=="right"?1.0F:-1.0F;
            if(s.displaceField==1)s.displace.subdivisionLevels=std::uint32_t(std::clamp(int(s.displace.subdivisionLevels)+int(direction),0,4));
            else if(s.displaceField==2)s.displace.midpoint=std::clamp(s.displace.midpoint+direction*.05F,0.0F,1.0F);
            else s.displace.strengthMeters=std::clamp(s.displace.strengthMeters*(direction>0?1.25F:0.8F),0.0001F,10.0F);
            update=true;
        }
        if(update) {s.typed.clear();s.displaceReplaceTyped=true;update_displace(s,*object);return true;}
        if(key=="backspace") {if(s.displaceReplaceTyped){s.typed.clear();s.displaceReplaceTyped=false;}else if(!s.typed.empty())s.typed.pop_back();}
        else if((key.size()==1&&((key[0]>='0'&&key[0]<='9')||key[0]=='.'))||key=="period") {
            if(s.displaceReplaceTyped){s.typed.clear();s.displaceReplaceTyped=false;}
            if(s.typed.size()<16)s.typed+=key=="period"?".":std::string(key);
        } else return true;
        char* end{};const float value=std::strtof(s.typed.c_str(),&end);
        if(s.typed.empty()||end!=s.typed.c_str()+s.typed.size()||!std::isfinite(value)) {s.displaceInputValid=false;s.result.error="Enter a finite number for the highlighted option";return true;}
        const bool valid=s.displaceField==1?value>=0 && value<=4 && std::floor(value)==value:
            s.displaceField==2?value>=0 && value<=1:value>0 && value<=10;
        if(!valid){s.displaceInputValid=false;s.result.error="Strength >0 and <=10 m, Subdivisions 0..4, Midpoint 0..1";return true;}
        if(s.displaceField==0)s.displace.strengthMeters=value;else if(s.displaceField==1)s.displace.subdivisionLevels=std::uint32_t(value);else s.displace.midpoint=value;
        update_displace(s,*object);return true;
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Boolean){
        if(control)return false;
        if(key=="escape"){s.kind=NativeMeshAuthoringState::Kind::None;return true;}
        if(key=="r"){s.replace=!s.replace;return true;}
        bool update=false;if(key=="tab"){std::swap(s.object,s.operand);object=workspace_.document().find_object(s.object);update=true;}
        else if(key=="u"||key=="d"||key=="x"||key=="i"){s.boolean=key=="u"?MeshBoolean::Union:key=="d"?MeshBoolean::Difference:key=="x"?MeshBoolean::ReverseDifference:MeshBoolean::Intersection;update=true;}
        bool apply=key=="enter"||key=="return";
        if(update||apply){const auto current=stamp(workspace_.document(),s.object,s.operand);if(update||current!=s.operandStamp){s.result=boolean_editor_polygons(workspace_.document(),s.object,s.operand,s.boolean);s.operandStamp=current;s.booleanPose=workspace_.document().world_transform(s.object).value_or(object->transform);s.booleanMetric=object->voxelScale;
                if(!s.result)set_status(s.result.error,true);else if(apply)set_status("Operands changed: inspect updated preview and press Enter again");return true;}}
        if(apply){if(!s.result){set_status(s.result.error,true);return true;}const auto before=capture_selection_snapshot();std::unique_ptr<IEditorCommand> command;EditorObjectId id=s.object;
            if(s.replace&&exact_brep_source(*object)){set_status(object->name+" is a CAD object: replacing it would break its exact link. Press R for a new result object, or use Exact Boolean",true);return true;}
            if(s.replace)command=std::make_unique<ReplacePolygonCommand>(id,*object->polygon,s.result.mesh,"Mesh Boolean");
            else {id=workspace_.document().allocate_object_id();EditorObject result(id,object->name+" (Boolean)");result.polygon=s.result.mesh;result.polygon->objectId=id;refresh_editor_polygon(*result.polygon);result.transform=s.booleanPose;result.voxelScale=s.booleanMetric;result.flags=object->flags;result.parent=object->parent;command=std::make_unique<AddObjectCommand>(std::move(result),"Create mesh Boolean result");}
            auto result=workspace_.commands().execute(workspace_.document(),std::move(command));if(result.success){if(!viewportSettings_.isolatedObjects.empty())viewportSettings_.isolatedObjects.insert(id);workspace_.select_object(id);selectionDomain_=SelectionDomain::Objects;workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());s.kind=NativeMeshAuthoringState::Kind::None;recompute_layout();}set_status(result.success?"Applied mesh Boolean":result.message,!result.success);return true;}
        return true;
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Hull){
        if(control){if(key=="z")cancel_mesh_authoring_drag();return false;}
        auto points=s.hullBefore?s.hullPreview:hull_points(*object);if(points.empty()){s.kind=NativeMeshAuthoringState::Kind::None;return false;}
        std::erase_if(s.hullSelection,[&](auto i){return i>=points.size();});
        auto apply=[&](std::vector<Float3> edited){auto hull=convex_editor_hull(object->id,edited);if(!hull){set_status(hull.error,true);return false;}auto result=workspace_.commands().execute(workspace_.document(),hull_command(workspace_.document(),*object,std::move(edited)));set_status(result.success?"Edited collision hull; visible mesh unchanged":result.message,!result.success);return result.success;};
        if(key=="escape"){if(s.hullBefore)cancel_mesh_authoring_drag();else s.kind=NativeMeshAuthoringState::Kind::None;return true;}
        if(key=="a"){s.hullSelection.clear();for(std::uint32_t i=0;i<points.size();++i)s.hullSelection.insert(i);return true;}
        if(key=="x"||key=="y"||key=="z"){s.axis=key=="x"?0:key=="y"?1:2;return true;}
        if(key=="g"||key=="r"||key=="s"){s.tool=key=="g"?1:key=="r"?2:3;return true;}
        if(key=="f"){
            EditorObjectBounds bounds;for(auto p:points){auto world=voxel_to_world(object->transform,object->voxelScale,p);if(!bounds.valid)bounds={world,world,true};else{bounds.minimum={std::min(bounds.minimum.x,world.x),std::min(bounds.minimum.y,world.y),std::min(bounds.minimum.z,world.z)};bounds.maximum={std::max(bounds.maximum.x,world.x),std::max(bounds.maximum.y,world.y),std::max(bounds.maximum.z,world.z)};}}frame_camera_on_bounds(camera_,bounds);return true;
        }
        if(key=="n"){if(points.size()>=256){set_status("Hull has the maximum 256 control points",true);return true;}auto p=points[s.hullSelection.empty()?0:*s.hullSelection.begin()];if(s.axis==0)p.x+=.1F;else if(s.axis==1)p.y+=.1F;else p.z+=.1F;points.push_back(p);if(apply(points))s.hullSelection={std::uint32_t(points.size()-1)};return true;}
        if(key=="delete"){std::vector<Float3> kept;for(std::uint32_t i=0;i<points.size();++i)if(!s.hullSelection.contains(i))kept.push_back(points[i]);if(apply(kept))s.hullSelection={0};return true;}
        if(key=="b"){auto hull=convex_editor_hull(object->id,points);if(hull){std::vector<Float3> bounds;for(unsigned i=0;i<8;++i)bounds.push_back({i&1?hull.mesh.bounds.maximum.x:hull.mesh.bounds.minimum.x,i&2?hull.mesh.bounds.maximum.y:hull.mesh.bounds.minimum.y,i&4?hull.mesh.bounds.maximum.z:hull.mesh.bounds.minimum.z});(void)apply(bounds);s.hullSelection={0};}return true;}
        if(key=="up"||key=="down"){float delta=key=="up"?.1F:-.1F;for(auto i:s.hullSelection){if(s.axis==0)points[i].x+=delta;else if(s.axis==1)points[i].y+=delta;else points[i].z+=delta;}(void)apply(points);return true;}
        if(key=="return"||key=="enter"){if(s.hullBefore&&s.hullValid){auto edited=s.hullPreview;cancel_mesh_authoring_drag();(void)apply(std::move(edited));}return true;}
        bool typed=key.size()==1&&((key[0]>='0'&&key[0]<='9')||key[0]=='.'||key[0]=='-');if(key=="period"||key=="minus")typed=true;
        if(typed||key=="backspace"){
            if(s.hullSelection.empty())return true;if(!s.hullBefore){s.hullBefore=points;s.hullPreview=points;s.typed.clear();}
            if(key=="backspace"){if(!s.typed.empty())s.typed.pop_back();}else if(s.typed.size()<16)s.typed+=key=="period"?".":key=="minus"?"-":std::string(key);
            char* end{};float value=std::strtof(s.typed.c_str(),&end);s.hullValid=!s.typed.empty()&&end==s.typed.c_str()+s.typed.size()&&std::isfinite(value);if(!s.hullValid)return true;
            s.hullPreview=*s.hullBefore;Float3 pivot{};for(auto i:s.hullSelection)pivot=add(pivot,s.hullPreview[i]);pivot=multiply(pivot,1.0F/float(s.hullSelection.size()));Float3 axis=s.axis==0?Float3{1,0,0}:s.axis==1?Float3{0,1,0}:Float3{0,0,1};
            if(s.tool==3&&!(value>0)){s.hullValid=false;return true;}
            for(auto i:s.hullSelection){auto& p=s.hullPreview[i];if(s.tool==1)p=add(p,multiply(axis,value));else if(s.tool==2)p=add(pivot,rotate(quaternion_from_axis_angle(axis,value*.01745329252F),subtract(p,pivot)));else if(s.tool==3)p=add(pivot,multiply(subtract(p,pivot),value));else {if(s.axis==0)p.x=value;else if(s.axis==1)p.y=value;else p.z=value;}}
            s.hullValid=bool(convex_editor_hull(object->id,s.hullPreview));return true;
        }
        return false;
    }
    if(s.kind!=NativeMeshAuthoringState::Kind::Uv||!s.uvFocus)return false;
    if(s.uvDensityTyping) {
        if(key=="escape"){s.uvDensityTyping=false;s.uvDensityText.clear();return true;}
        if(key=="return"||key=="enter"){
            char* end{};
            const float density=std::strtof(s.uvDensityText.c_str(),&end);
            if(s.uvDensityText.empty()||end!=s.uvDensityText.c_str()+s.uvDensityText.size()||
               !std::isfinite(density)||!(density>0)||density>1000000.0F)
               {set_status("Density target must be a number in (0, 1,000,000] texels/m",true);return true;}
            s.uvDensityTarget=density;s.uvDensityTyping=false;s.uvDensityText.clear();
            set_status("UV density target: "+std::to_string(density)+" texels per metre");return true;
        }
        if(key=="backspace"){if(!s.uvDensityText.empty())s.uvDensityText.pop_back();return true;}
        const bool digit=key.size()==1&&(key[0]>='0'&&key[0]<='9');
        if(digit||key=="period"||key=="."){
            if(s.uvDensityText.size()<14)s.uvDensityText+=key=="period"?".":std::string(key);
        }
        return true;
    }
    (void)refresh_uv_flags(s,*object);
    std::erase_if(s.corners,[&](auto c){return c>=object->polygon->indices.size();});
    if(control&&key=="z"){cancel_mesh_authoring_drag();return false;}
    if(control&&key=="a"){s.corners.clear();for(std::uint32_t c=0;c<object->polygon->indices.size();++c)s.corners.insert(c);polygonSelection_={s.object,PolygonSelectionMode::Face,{}};for(auto c:s.corners)polygonSelection_.components.insert(c/3);return true;}
    if(control)return false;
    if(key=="escape"){if(s.uvBefore)cancel_mesh_authoring_drag();else s.kind=NativeMeshAuthoringState::Kind::None;return true;}
    if(key=="1"||key=="2"||key=="3"||key=="4"){if(!s.uvBefore&&s.tool==0){s.uvMode=unsigned(key[0]-'1');return true;}}
    if(key=="tab"){cancel_mesh_authoring_drag();s.channel=1-s.channel;s.corners.clear();return true;}
    if(key=="q"){cancel_mesh_authoring_drag();s.tool=0;return true;}
    if(key=="g"||key=="r"||key=="s"){cancel_mesh_authoring_drag();s.tool=key=="g"?1:key=="r"?2:3;s.axis=0;return true;}
    if(key=="p" || key=="m") {
        if(workspace_.selected_object()!=s.object) { set_status("Select the UV workspace object first",true); return true; }
        (void)dispatch_mesh_authoring_action(key=="p"?"mesh.uv_toggle_pin":shift?"mesh.uv_clear_seam":"mesh.uv_mark_seam");
        return true;
    }
    if(key=="t"){s.texture=!s.texture;return true;}
    if(key=="plus"||key=="equal"||key=="minus"){if(!s.uvBefore && (key!="minus" || s.tool==0)){s.zoom=std::clamp(s.zoom*(key=="minus"?.8F:1.25F),.05F,64.0F);return true;}}
    if(key=="f"){
        Float2 minimum{INFINITY,INFINITY},maximum{-INFINITY,-INFINITY};auto points=s.corners;if(points.empty())for(std::uint32_t c=0;c<object->polygon->indices.size();++c)points.insert(c);
        for(auto c:points){auto p=texcoord(object->polygon->vertices[object->polygon->indices[c]],s.channel);minimum={std::min(minimum.x,p.x),std::min(minimum.y,p.y)};maximum={std::max(maximum.x,p.x),std::max(maximum.y,p.y)};}
        s.center={minimum.x*.5F+maximum.x*.5F,minimum.y*.5F+maximum.y*.5F};s.zoom=std::clamp(1.0F/std::max(.05F,std::max(maximum.x-minimum.x,maximum.y-minimum.y)),.05F,64.0F);return true;
    }
    if(key=="x"||key=="y"){s.axis=key=="x"?1:2;return true;}
    const auto uvSettings=[&] {
        UvEditSettings settings;settings.channel=s.channel;settings.objectScale=object->voxelScale;
        settings.targetTexelsPerMeter=s.uvDensityTarget;
        if(!refresh_uv_flags(s,*object))return settings;
        if(s.uvFlags.enabled) {
            settings.seams=s.uvFlags.seams[s.channel];
            for(const auto corner:s.uvFlags.pins[s.channel])
                if(corner<object->polygon->indices.size())
                    settings.pins[corner]=texcoord(object->polygon->vertices[object->polygon->indices[corner]],s.channel);
        }
        if(!object->polygon->materialBindings.empty()){
            const auto texture=object->polygon->materialBindings[0].baseColor.texture;
            if(texture && *texture<object->polygon->textures.size()) {
                const auto image=object->polygon->textures[*texture].imageIndex;
                if(image<object->polygon->images.size()) {
                    const auto& picture=object->polygon->images[image];
                    if(picture.width && picture.height) {
                        settings.textureWidth=picture.width;settings.textureHeight=picture.height;
                    }
                }
            }
        }
        return settings;
    };
    if(key=="i") {
        if(s.uvBefore){set_status("Apply/cancel UV preview before editing density target",true);return true;}
        s.uvDensityTyping=true;s.uvDensityText.clear();
        set_status("Type target density in texels per metre, Enter confirm, Esc cancel");return true;
    }
    if(key=="b") {
        if(s.uvBefore){set_status("Apply/cancel UV preview before measuring density",true);return true;}
        const auto density=measure_polygon_uv_density(*object->polygon,s.corners,uvSettings());
        if(!density){set_status(density.error,true);return true;}
        if(shift)s.uvDensityTarget=float(density.texelsPerMeter);
        set_status("UV"+std::to_string(s.channel)+": "+
            std::to_string(density.texelsPerMeter)+" texels/m across "+
            std::to_string(density.charts.size())+" selected islands"+
            (shift?" | captured as match target":""),false,8.0F);return true;
    }
    if(key=="n"||key=="o"||key=="l"||key=="c") {
        if(s.corners.empty()){set_status("Select UV corners/islands first",true);return true;}
        if(s.uvBefore){set_status("Apply/cancel UV preview before a new operation",true);return true;}
        if(!refresh_uv_flags(s,*object)){set_status(s.uvFlagsError,true);return true;}
        const auto operation=key=="n"?UvEdit::MatchDensity:
            key=="o"?UvEdit::AlignIslandU:key=="l"?UvEdit::AlignIslandV:
            shift?UvEdit::StraightenV:UvEdit::StraightenU;
        const auto before=*object->polygon;
        const auto result=edit_polygon_uv(before,s.corners,operation,uvSettings());
        if(!result){set_status(result.error,true);return true;}
        s.uvBefore=before;*object->polygon=result.mesh;
        set_status("UV preview: Enter apply one Undo step, Esc cancel; target "+
            std::to_string(s.uvDensityTarget)+" texels/m");return true;
    }
    auto commit=[&](PolygonEditResult edited,const CookedPolygonAsset& before){if(!edited){set_status(edited.error,true);return false;}auto snapshot=capture_selection_snapshot();auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<ReplacePolygonCommand>(object->id,before,std::move(edited.mesh),"Edit UV coordinates"));if(result.success)workspace_.commands().set_last_selection_transition(snapshot,capture_selection_snapshot());set_status(result.success?"Edited UV coordinates":result.message,!result.success);return result.success;};
    if(key=="u"||key=="k"||key=="h"||key=="v"||key=="j"||key=="d"){
        cancel_mesh_authoring_drag();UvEditSettings settings;settings.channel=s.channel;
        if((key=="u" || key=="k") && !refresh_uv_flags(s,*object)) { set_status(s.uvFlagsError,true); return true; }
        if(s.uvFlags.enabled)settings.seams=s.uvFlags.seams[s.channel];
        const auto pins=s.uvFlags.enabled?s.uvFlags.pins[s.channel]:std::set<std::uint32_t>{};
        for(auto c:pins)if(c<object->polygon->indices.size())settings.pins[c]=texcoord(object->polygon->vertices[object->polygon->indices[c]],s.channel);if(key=="d")settings.vector={.05F,0};auto op=key=="u"?UvEdit::Unwrap:key=="k"?UvEdit::Pack:key=="h"?UvEdit::FlipU:key=="v"?UvEdit::FlipV:key=="j"?UvEdit::Stitch:UvEdit::Split;
        (void)commit(edit_polygon_uv(*object->polygon,s.corners,op,settings),*object->polygon);return true;
    }
    if(key=="enter"||key=="return"){if(s.uvBefore){auto edited=*object->polygon, before=*s.uvBefore;cancel_mesh_authoring_drag();if(edited.contentHash!=before.contentHash){PolygonEditResult r;r.mesh=std::move(edited);(void)commit(std::move(r),before);}}return true;}
    bool typed=key.size()==1&&((key[0]>='0'&&key[0]<='9')||key[0]=='.'||key[0]=='-');if(key=="period"||key=="minus")typed=true;
    if(s.tool&&(typed||key=="backspace")){
        if(s.corners.empty()){set_status("Select UV components first",true);return true;}
        if(!s.uvBefore){s.uvBefore=*object->polygon;s.typed.clear();s.pivot={};std::set<std::uint32_t> vertices;for(auto c:s.corners)if(c<object->polygon->indices.size())vertices.insert(object->polygon->indices[c]);for(auto v:vertices){auto p=texcoord(object->polygon->vertices[v],s.channel);s.pivot.x+=p.x;s.pivot.y+=p.y;}s.pivot.x/=float(vertices.size());s.pivot.y/=float(vertices.size());}
        if(key=="backspace"){if(!s.typed.empty())s.typed.pop_back();}else if(s.typed.size()<16)s.typed+=key=="period"?".":key=="minus"?"-":std::string(key);
        char* end{};float value=std::strtof(s.typed.c_str(),&end);*object->polygon=*s.uvBefore;if(s.typed.empty()||end!=s.typed.c_str()+s.typed.size()||!std::isfinite(value))return true;
        UvEditSettings settings;settings.channel=s.channel;settings.pivot=s.pivot;
        if(s.tool==1)settings.vector=s.axis==2?Float2{0,value}:Float2{value,0};else if(s.tool==2)settings.angle=value*.01745329252F;else settings.vector=s.axis==1?Float2{value,1}:s.axis==2?Float2{1,value}:Float2{value,value};
        auto edited=edit_polygon_uv(*s.uvBefore,s.corners,s.tool==1?UvEdit::Translate:s.tool==2?UvEdit::Rotate:UvEdit::Scale,settings);if(edited)*object->polygon=std::move(edited.mesh);else set_status(edited.error,true);return true;
    }
    if(key=="a"){s.corners.clear();for(std::uint32_t c=0;c<object->polygon->indices.size();++c)s.corners.insert(c);polygonSelection_={s.object,PolygonSelectionMode::Face,{}};for(auto c:s.corners)polygonSelection_.components.insert(c/3);return true;}
    return false;
}
bool NativeEditorController::mesh_authoring_pointer_down(PointerButton button,int x,int y,std::uint32_t modifiers){
    if(!meshAuthoring_||workspace_.mode()!=EditorMode::Edit||playSession_.active()||(modifiers&4U))return false;auto& s=*meshAuthoring_;auto* object=workspace_.document().find_object(s.object);if(!object||!object->polygon||object->flags.locked)return false;
    if(s.kind==NativeMeshAuthoringState::Kind::Topology && button==PointerButton::Primary && layout_.viewport.contains(x,y)) {
        if(!object->flags.visible||is_isolated_out(s.object)){cancel_mesh_authoring_drag();s.kind=NativeMeshAuthoringState::Kind::None;set_status("Bevel preview closed: object is hidden or isolated out",true);return true;}
        const auto view=layout_.viewport;
        for(int i=0;i<3;++i) if(bevel_button(view,0,i).contains(x,y)) {
            if(i==0)(void)mesh_authoring_key("c",false,false,false);
            else if(i==1)(void)mesh_authoring_key("r",false,false,false);
            else {s.bevel=EdgeBevelSettings{};s.typed.clear();s.bevelReplaceTyped=true;update_bevel(s,*object);}
            return true;
        }
        for(int i=0;i<3;++i) if(bevel_button(view,1,i).contains(x,y)) {
            (void)mesh_authoring_key(i==0?"f":i==1?"w":"a",false,false,false);return true;
        }
        for(int i=0;i<4;++i) if(bevel_field(view,i).contains(x,y)) {
            s.bevelField=i;s.typed=bevel_value(s,i);s.bevelReplaceTyped=true;update_bevel(s,*object);return true;
        }
        const auto panel=bevel_panel(view);const int width=std::max(1,(panel.width-16)/2);
        if(UiRect{panel.x+8,panel.y+191,width-3,23}.contains(x,y))(void)mesh_authoring_key("enter",false,false,false);
        else if(UiRect{panel.x+8+width,panel.y+191,width-3,23}.contains(x,y))(void)mesh_authoring_key("escape",false,false,false);
        return true; // Preview owns primary authoring input; Alt/non-primary camera gestures still work.
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Displace && button==PointerButton::Primary && layout_.viewport.contains(x,y)) {
        if(!object->flags.visible||is_isolated_out(s.object)){cancel_mesh_authoring_drag();s.kind=NativeMeshAuthoringState::Kind::None;set_status("Displacement preview closed: object is hidden or isolated out",true);return true;}
        const auto view=layout_.viewport;
        const float strengths[]={0.005F,0.02F,0.08F};
        for(int i=0;i<3;++i) if(bevel_button(view,0,i).contains(x,y)) {
            s.displace.strengthMeters=strengths[i];s.typed.clear();s.displaceReplaceTyped=true;update_displace(s,*object);return true;
        }
        const std::uint32_t levels[]={0U,2U,4U};
        for(int i=0;i<3;++i) if(bevel_button(view,1,i).contains(x,y)) {
            s.displace.subdivisionLevels=levels[i];s.typed.clear();s.displaceReplaceTyped=true;update_displace(s,*object);return true;
        }
        for(int i=0;i<3;++i) if(bevel_field(view,i).contains(x,y)) {
            s.displaceField=i;s.typed=displace_value(s,i);s.displaceReplaceTyped=true;update_displace(s,*object);return true;
        }
        const auto panel=bevel_panel(view);const int width=std::max(1,(panel.width-16)/2);
        if(UiRect{panel.x+8,panel.y+191,width-3,23}.contains(x,y))(void)mesh_authoring_key("enter",false,false,false);
        else if(UiRect{panel.x+8+width,panel.y+191,width-3,23}.contains(x,y))(void)mesh_authoring_key("escape",false,false,false);
        return true; // Preview owns primary authoring input, like the bevel preview.
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Hull&&button==PointerButton::Primary&&layout_.viewport.contains(x,y)){
        auto points=hull_points(*object);float best=10;std::optional<std::uint32_t> chosen;for(std::uint32_t i=0;i<points.size();++i){auto p=project_world_to_screen(camera_,layout_.viewport,voxel_to_world(object->transform,object->voxelScale,points[i]));float distance=std::hypot(float(x)-p.x,float(y)-p.y);if(p.visible&&distance<best){best=distance;chosen=i;}}
        if(chosen){cancel_mesh_authoring_drag();if((modifiers&1U)==0)s.hullSelection.clear();if((modifiers&2U)&&s.hullSelection.contains(*chosen))s.hullSelection.erase(*chosen);else s.hullSelection.insert(*chosen);return true;}return false;
    }
    if(s.kind!=NativeMeshAuthoringState::Kind::Uv)return false;
    (void)refresh_uv_flags(s,*object);
    auto panel=uv_panel(layout_.viewport);if(!panel.contains(x,y)){if(s.uvBefore&&button==PointerButton::Primary){set_status("Apply or cancel the UV preview first",true);return true;}s.uvFocus=false;return false;}
    if(!s.uvFocus&&polygonSelection_.object==s.object&&polygonSelection_.mode==PolygonSelectionMode::Face){s.corners.clear();for(auto f:polygonSelection_.components)for(unsigned j=0;j<3;++j)s.corners.insert(std::uint32_t(f*3+j));}s.uvFocus=true;
    if(button==PointerButton::Primary){
        const int modeWidth=std::max(1,(panel.width-16)/4),actionWidth=std::max(1,(panel.width-16)/5);
        for(unsigned i=0;i<4;++i)if(UiRect{panel.x+8+int(i)*modeWidth,panel.y+28,modeWidth-2,20}.contains(x,y)){cancel_mesh_authoring_drag();s.uvMode=i;s.tool=0;return true;}
        for(unsigned i=0;i<5;++i)if(UiRect{panel.x+8+int(i)*actionWidth,panel.y+50,actionWidth-2,20}.contains(x,y)){const char* key[]={"g","r","s","u","k"};(void)mesh_authoring_key(key[i],false,false,false);return true;}
        for(unsigned i=0;i<5;++i)if(UiRect{panel.x+8+int(i)*actionWidth,panel.y+72,actionWidth-2,20}.contains(x,y)){const char* key[]={"b","i","n","o","l"};(void)mesh_authoring_key(key[i],false,false,false);return true;}
    }
    if(!uv_canvas(layout_.viewport).contains(x,y))return true;
    s.startX=s.lastX=x;s.startY=s.lastY=y;s.startUv=from_screen(x,y,s,layout_.viewport);
    if(button==PointerButton::Auxiliary){s.panning=true;s.panStart=s.center;return true;}
    if(button!=PointerButton::Primary)return true;
    if(s.tool&&!s.corners.empty()){
        cancel_mesh_authoring_drag();s.uvBefore=*object->polygon;s.dragging=true;s.pivot={};std::set<std::uint32_t> vertices;for(auto c:s.corners)if(c<object->polygon->indices.size())vertices.insert(object->polygon->indices[c]);
        if(vertices.empty()){cancel_mesh_authoring_drag();return true;}for(auto v:vertices){auto p=texcoord(object->polygon->vertices[v],s.channel);s.pivot.x+=p.x;s.pivot.y+=p.y;}s.pivot.x/=float(vertices.size());s.pivot.y/=float(vertices.size());return true;
    }
    s.selecting=true;s.selectionOperation=selection_operation_for_modifiers(modifiers);return true;
}
bool NativeEditorController::mesh_authoring_pointer_move(int x,int y,std::uint32_t modifiers){
    if(!meshAuthoring_||meshAuthoring_->kind!=NativeMeshAuthoringState::Kind::Uv)return false;auto& s=*meshAuthoring_;if(!s.dragging&&!s.panning&&!s.selecting)return false;
    auto* object=workspace_.document().find_object(s.object);if(!object||!object->polygon){cancel_mesh_authoring_drag();return true;}
    s.lastX=x;s.lastY=y;auto current=from_screen(x,y,s,layout_.viewport);
    if(s.panning){auto start=from_screen(s.startX,s.startY,s,layout_.viewport);s.center={s.panStart.x+start.x-current.x,s.panStart.y+start.y-current.y};return true;}
    if(s.selecting)return true;
    *object->polygon=*s.uvBefore;UvEditSettings settings;settings.channel=s.channel;settings.pivot=s.pivot;auto operation=s.tool==1?UvEdit::Translate:s.tool==2?UvEdit::Rotate:UvEdit::Scale;
    if(s.tool==1){settings.vector={current.x-s.startUv.x,current.y-s.startUv.y};if(s.axis==1)settings.vector.y=0;if(s.axis==2)settings.vector.x=0;if(modifiers&2U){settings.vector.x=std::round(settings.vector.x*10)/10;settings.vector.y=std::round(settings.vector.y*10)/10;}if(settings.vector.x==0&&settings.vector.y==0)return true;}
    else if(s.tool==2){settings.angle=std::atan2(current.y-s.pivot.y,current.x-s.pivot.x)-std::atan2(s.startUv.y-s.pivot.y,s.startUv.x-s.pivot.x);if(modifiers&2U)settings.angle=std::round(settings.angle/.261799F)*.261799F;if(settings.angle==0)return true;}
    else{float factor=std::clamp(std::exp(float(x-s.startX)*.01F),.01F,100.0F);settings.vector=s.axis==1?Float2{factor,1}:s.axis==2?Float2{1,factor}:Float2{factor,factor};if(factor==1)return true;}
    auto result=edit_polygon_uv(*s.uvBefore,s.corners,operation,settings);if(result)*object->polygon=std::move(result.mesh);else set_status(result.error,true);return true;
}
bool NativeEditorController::mesh_authoring_pointer_up(PointerButton button,int x,int y,std::uint32_t modifiers){
    if(!meshAuthoring_||meshAuthoring_->kind!=NativeMeshAuthoringState::Kind::Uv)return false;auto& s=*meshAuthoring_;if(s.panning){s.panning=false;return true;}if(button!=PointerButton::Primary)return false;
    if(s.dragging){(void)mesh_authoring_pointer_move(x,y,modifiers);(void)mesh_authoring_key("return",false,false,false);return true;}
    if(!s.selecting)return false;s.selecting=false;auto* object=workspace_.document().find_object(s.object);if(!object||!object->polygon)return true;std::set<std::uint32_t> hits;
    if(std::hypot(float(x-s.startX),float(y-s.startY))<=5)hits=uv_click(*object->polygon,s,layout_.viewport,x,y);
    else{UiRect rect{std::min(x,s.startX),std::min(y,s.startY),std::abs(x-s.startX)+1,std::abs(y-s.startY)+1};for(std::uint32_t c=0;c<object->polygon->indices.size();++c){auto p=screen_uv(texcoord(object->polygon->vertices[object->polygon->indices[c]],s.channel),s,layout_.viewport);if(rect.contains(int(p.x),int(p.y)))hits.insert(c);}
        if(s.uvMode>=2){std::set<std::uint32_t> faces;for(auto c:hits)faces.insert(c/3);if(s.uvMode==3)for(auto island:polygon_uv_islands(*object->polygon,s.channel,s.uvFlags.enabled?s.uvFlags.seams[s.channel]:std::set<std::uint64_t>{})){bool selected=std::any_of(island.begin(),island.end(),[&](auto f){return faces.contains(f);});if(selected)faces.insert(island.begin(),island.end());}hits.clear();for(auto f:faces)for(unsigned j=0;j<3;++j)hits.insert(f*3+j);}}
    if(s.selectionOperation==SelectionOperation::Replace)s.corners.clear();if(s.selectionOperation==SelectionOperation::Intersect)std::erase_if(s.corners,[&](auto c){return !hits.contains(c);});else for(auto c:hits){if(s.selectionOperation==SelectionOperation::Subtract||(s.selectionOperation==SelectionOperation::Toggle&&s.corners.contains(c)))s.corners.erase(c);else s.corners.insert(c);}
    polygonSelection_={s.object,PolygonSelectionMode::Face,{}};for(auto c:s.corners)polygonSelection_.components.insert(c/3);selectionDomain_=SelectionDomain::Polygons;return true;
}

bool NativeEditorController::mesh_authoring_wheel(float steps,int x,int y){
    if(!meshAuthoring_||meshAuthoring_->kind!=NativeMeshAuthoringState::Kind::Uv||!uv_canvas(layout_.viewport).contains(x,y)||meshAuthoring_->uvBefore)return false;
    if(!std::isfinite(steps))return true;auto& state=*meshAuthoring_;auto before=from_screen(x,y,state,layout_.viewport);state.zoom=std::clamp(state.zoom*std::pow(1.2F,steps),.05F,64.0F);
    auto after=from_screen(x,y,state,layout_.viewport);state.center.x+=before.x-after.x;state.center.y+=before.y-after.y;return true;
}
void NativeEditorController::draw_mesh_authoring(const IEditorCanvas& canvas,UiRect viewport) const{
    if (voxel_authoring_active()) { draw_voxel_authoring(canvas,viewport); return; }
    draw_subdivision_cage(canvas,viewport);
    if(!meshAuthoring_||workspace_.mode()!=EditorMode::Edit||playSession_.active())return;auto& s=*meshAuthoring_;const auto* object=workspace_.document().find_object(s.object);if(!object||!object->polygon||!object->flags.visible||is_isolated_out(s.object))return;
    auto line=[&](ScreenPoint a,ScreenPoint b,EditorColor c,int width=1){if(a.visible&&b.visible)canvas.line(int(std::clamp(a.x,-100000.0F,100000.0F)),int(std::clamp(a.y,-100000.0F,100000.0F)),int(std::clamp(b.x,-100000.0F,100000.0F)),int(std::clamp(b.y,-100000.0F,100000.0F)),c,width);};
    if(s.diagnostics){auto d=diagnose_polygon_topology(*object->polygon);for(auto e:polygon_edges(*object->polygon))if(d.problemEdges.contains(e.key())){auto a=project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,object->polygon->vertices[e.a].position)),b=project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,object->polygon->vertices[e.b].position));line(a,b,color(255,55,80),3);}}
    if(s.showUvSeams && (s.kind==NativeMeshAuthoringState::Kind::None || s.kind==NativeMeshAuthoringState::Kind::Uv) &&
       refresh_uv_flags(s,*object) && s.uvFlags.enabled) {
        for(const auto edge : polygon_edges(*object->polygon)) if(s.uvFlags.seams[s.channel].contains(edge.key()))
            line(project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,
                    object->polygon->vertices[edge.a].position)),
                 project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,
                    object->polygon->vertices[edge.b].position)),color(235,95,85),3);
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Topology){
        if(s.result){EditorDocument preview("Bevel preview");EditorObject o(s.object,"Result");o.polygon=s.result.mesh;o.subdivision=s.bevelAuthored.subdivision;o.transform=object->transform;o.voxelScale=object->voxelScale;preview.add_object(std::move(o));for(const auto& t:build_editor_polygon_draw_list(preview,camera_,viewport,{}, {},20000))for(unsigned j=0;j<3;++j)line(t.points[j],t.points[(j+1)%3],color(255,175,55),2);}
        const auto panel=bevel_panel(viewport);RectClipCanvas painter(canvas,panel);painter.fill(panel,color(23,32,43));
        const auto label=[&](UiRect rect,std::string text,EditorColor shade){painter.text(rect.x+5,rect.y+16,elide_text_to_width(painter,text,std::max(1,rect.width-10)),shade);};
        label({panel.x+8,panel.y+3,panel.width-16,22},"Edge Bevel / Chamfer / Rounded Fillet",color(255,210,140));
        const char* presets[]={"Chamfer","Rounded","Reset"};
        for(int i=0;i<3;++i){const auto button=bevel_button(viewport,0,i);painter.fill(button,color(38,48,64));label(button,presets[i],color(225,235,245));}
        const std::string toggles[]={s.bevel.offsetType==BevelOffsetType::Fractional?"Fractional":"Absolute",
            s.bevel.offsetType==BevelOffsetType::Fractional?"Local fraction":s.bevel.worldSpace?"World space":"Local space",s.bevel.autoFit?"Auto Fit: on":"Auto Fit: off"};
        for(int i=0;i<3;++i){const auto button=bevel_button(viewport,1,i);painter.fill(button,color(38,48,64));label(button,toggles[i],color(225,235,245));}
        const char* fields[]={"Width","Segments","Depth / Profile","Smoothing Angle"};
        for(int i=0;i<4;++i){const auto field=bevel_field(viewport,i);painter.fill(field,s.bevelField==i?color(66,82,106):color(32,42,56));
            label(field,std::string(fields[i])+": "+(s.bevelField==i&&!s.typed.empty()?s.typed:bevel_value(s,i)),color(225,235,245));}
        const int width=std::max(1,(panel.width-16)/2);const UiRect apply{panel.x+8,panel.y+191,width-3,23},cancel{panel.x+8+width,panel.y+191,width-3,23};
        painter.fill(apply,s.result?color(40,92,64):color(68,44,44));painter.fill(cancel,color(55,64,78));label(apply,"Apply (Enter)",color(225,235,245));label(cancel,"Cancel (Esc)",color(225,235,245));
        label({panel.x+8,panel.y+215,panel.width-16,23},s.result?"Tab: field | Arrows: adjust | C: chamfer | R: round":s.result.error,s.result?color(185,200,220):color(255,145,130));return;
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Displace){
        if(s.result){EditorDocument preview("Displacement preview");EditorObject o(s.object,"Result");o.polygon=s.result.mesh;o.transform=object->transform;o.voxelScale=object->voxelScale;preview.add_object(std::move(o));for(const auto& t:build_editor_polygon_draw_list(preview,camera_,viewport,{}, {},20000))for(unsigned j=0;j<3;++j)line(t.points[j],t.points[(j+1)%3],color(255,175,55),2);}
        const auto panel=bevel_panel(viewport);RectClipCanvas painter(canvas,panel);painter.fill(panel,color(23,32,43));
        const auto label=[&](UiRect rect,std::string text,EditorColor shade){painter.text(rect.x+5,rect.y+16,elide_text_to_width(painter,text,std::max(1,rect.width-10)),shade);};
        label({panel.x+8,panel.y+3,panel.width-16,22},"Displace from Height Texture (Bake)",color(255,210,140));
        const char* presets[]={"Strength 5 mm","Strength 20 mm","Strength 80 mm"};
        for(int i=0;i<3;++i){const auto button=bevel_button(viewport,0,i);painter.fill(button,color(38,48,64));label(button,presets[i],color(225,235,245));}
        const char* levelPresets[]={"Subdivide 0","Subdivide 2","Subdivide 4"};
        for(int i=0;i<3;++i){const auto button=bevel_button(viewport,1,i);painter.fill(button,color(38,48,64));label(button,levelPresets[i],color(225,235,245));}
        const char* fields[]={"Strength (m)","Subdivisions","Midpoint"};
        for(int i=0;i<3;++i){const auto field=bevel_field(viewport,i);painter.fill(field,s.displaceField==i?color(66,82,106):color(32,42,56));
            label(field,std::string(fields[i])+": "+(s.displaceField==i&&!s.typed.empty()?s.typed:displace_value(s,i)),color(225,235,245));}
        const int width=std::max(1,(panel.width-16)/2);const UiRect apply{panel.x+8,panel.y+191,width-3,23},cancel{panel.x+8+width,panel.y+191,width-3,23};
        painter.fill(apply,s.result?color(40,92,64):color(68,44,44));painter.fill(cancel,color(55,64,78));label(apply,"Apply (Enter)",color(225,235,245));label(cancel,"Cancel (Esc)",color(225,235,245));
        const std::string info=s.result?("Tab: field | Arrows: adjust | "+std::to_string(s.displaceStats.outputTriangles)+" tris | max "+std::to_string(s.displaceStats.maximumAbsoluteDisplacement)+" m"):s.result.error;
        label({panel.x+8,panel.y+215,panel.width-16,23},info,s.result?color(185,200,220):color(255,145,130));return;
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Boolean){
        if(s.result){EditorDocument preview("Boolean preview");EditorObject o(s.object,"Result");o.polygon=s.result.mesh;o.subdivision=s.replace?object->subdivision:std::optional<SubdivisionSettings>{};if(o.subdivision)*o.subdivision=remap_subdivision_topology(*object->polygon,*o.polygon,*o.subdivision);o.transform=s.booleanPose;o.voxelScale=s.booleanMetric;preview.add_object(std::move(o));for(const auto& triangle:build_editor_polygon_draw_list(preview,camera_,viewport,{}, {},20000))for(unsigned j=0;j<3;++j)line(triangle.points[j],triangle.points[(j+1)%3],color(255,175,55),2);}
        auto* b=workspace_.document().find_object(s.operand);canvas.fill({viewport.x+10,viewport.y+10,std::min(480,viewport.width-20),94},color(23,32,43));canvas.text(viewport.x+20,viewport.y+30,"Mesh Boolean | A: "+object->name+" | B: "+(b?b->name:"missing"),color(255,210,140));
        const char* op=s.boolean==MeshBoolean::Union?"Union":s.boolean==MeshBoolean::Difference?"A - B":s.boolean==MeshBoolean::ReverseDifference?"B - A":"Intersection";
        canvas.text(viewport.x+20,viewport.y+52,std::string(op)+(s.replace?" | Replace A (keep B)":" | New object (keep inputs)"),color(230,235,245));canvas.text(viewport.x+20,viewport.y+74,"U/D/X/I operation | Tab swap | R output",color(200,215,235));canvas.text(viewport.x+20,viewport.y+96,s.result?"Enter apply | Esc cancel":s.result.error,color(255,160,100));return;
    }
    if(s.kind==NativeMeshAuthoringState::Kind::Hull){auto points=s.hullBefore?s.hullPreview:hull_points(*object);auto hull=convex_editor_hull(object->id,points);if(hull)for(auto edge:polygon_edges(hull.mesh))line(project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,hull.mesh.vertices[edge.a].position)),project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,hull.mesh.vertices[edge.b].position)),color(60,230,235),2);
        for(std::uint32_t i=0;i<points.size();++i){auto p=project_world_to_screen(camera_,viewport,voxel_to_world(object->transform,object->voxelScale,points[i]));if(p.visible)canvas.fill({int(p.x)-4,int(p.y)-4,9,9},s.hullSelection.contains(i)?color(255,175,60):color(65,215,240));}
        canvas.fill({viewport.x+10,viewport.y+10,std::min(510,viewport.width-20),90},color(23,32,43));canvas.text(viewport.x+20,viewport.y+30,"Collision hull only | "+std::to_string(points.size())+" points | axis "+(s.axis==0?"X":s.axis==1?"Y":"Z"),color(80,230,240));canvas.text(viewport.x+20,viewport.y+51,"Click/Shift select | A all | N add | Delete remove | B bounds",color(220,230,240));canvas.text(viewport.x+20,viewport.y+72,"G move / R rotate degrees / S scale | X/Y/Z axis | type value",color(220,230,240));canvas.text(viewport.x+20,viewport.y+93,s.typed.empty()?"Enter apply typed edit | arrows nudge | Esc close":("Value: "+s.typed+(s.hullValid?" | Enter apply":" | invalid hull")),color(255,200,130));return;
    }
    if(s.kind!=NativeMeshAuthoringState::Kind::Uv)return;
    (void)refresh_uv_flags(s,*object);
    auto panel=uv_panel(viewport),rect=uv_canvas(viewport);RectClipCanvas painter(canvas,rect);canvas.fill(panel,color(20,27,36));canvas.text(panel.x+12,panel.y+23,"UV WORKSPACE | channel "+std::to_string(s.channel)+" | "+std::to_string(s.corners.size())+" corners"+(s.uvFlags.enabled?" | Tab channel":" | flags disabled"),color(120,215,255));const int modeWidth=std::max(1,(panel.width-16)/4),actionWidth=std::max(1,(panel.width-16)/5);
    const char* modes[]={"1 Vertex","2 Edge","3 Face","4 Island"};const char* actions[]={"G Move","R Rotate","S Scale","U Unwrap","K Pack"};const char* densityActions[]={"B Measure","I Target","N Match","O Align U","L Align V"};
    for(unsigned i=0;i<4;++i){UiRect button{panel.x+8+int(i)*modeWidth,panel.y+28,modeWidth-2,20};canvas.fill(button,s.uvMode==i?color(80,92,112):color(38,48,64));canvas.text(button.x+4,button.y+15,modes[i],color(225,230,240));}
    for(unsigned i=0;i<5;++i){UiRect button{panel.x+8+int(i)*actionWidth,panel.y+50,actionWidth-2,20};canvas.fill(button,color(38,48,64));canvas.text(button.x+4,button.y+15,actions[i],color(225,230,240));}
    for(unsigned i=0;i<5;++i){UiRect button{panel.x+8+int(i)*actionWidth,panel.y+72,actionWidth-2,20};canvas.fill(button,color(38,48,64));canvas.text(button.x+4,button.y+15,densityActions[i],color(225,230,240));}
    if(s.uvDensityTyping)canvas.text(panel.x+12,panel.y+panel.height-49,
        "Target texels/m: "+s.uvDensityText+" | Enter accept, Esc cancel",color(255,220,110));
    const PolygonImage* image=nullptr;if(s.texture&&!object->polygon->materialBindings.empty()){auto binding=object->polygon->materialBindings[0].baseColor.texture;if(binding&&*binding<object->polygon->textures.size()){auto index=object->polygon->textures[*binding].imageIndex;if(index<object->polygon->images.size())image=&object->polygon->images[index];}}
    constexpr int tile=12;for(int y=rect.y;y<rect.y+rect.height;y+=tile)for(int x=rect.x;x<rect.x+rect.width;x+=tile){auto p=from_screen(x+tile/2,y+tile/2,s,viewport);bool dark=std::fmod(std::floor(double(p.x)*8)+std::floor(double(p.y)*8),2.0)!=0;auto c=dark?color(46,53,64):color(69,76,88);
        if(image&&p.x>=0&&p.x<1&&p.y>=0&&p.y<1&&image->width&&image->height&&image->rgba8.size()>=std::size_t(image->width)*image->height*4){auto ix=std::min(image->width-1,std::uint32_t(p.x*image->width)),iy=std::min(image->height-1,std::uint32_t((1-p.y)*image->height));auto k=(std::size_t(iy)*image->width+ix)*4;c=color(image->rgba8[k],image->rgba8[k+1],image->rgba8[k+2]);}painter.fill({x,y,tile,tile},c);}
    std::array<Float2,4> unit{{{0,0},{1,0},{1,1},{0,1}}};for(unsigned j=0;j<4;++j){auto a=screen_uv(unit[j],s,viewport),b=screen_uv(unit[(j+1)%4],s,viewport);painter.line(int(a.x),int(a.y),int(b.x),int(b.y),color(245,200,95),2);}
    const auto& mesh=*object->polygon;std::set<std::uint32_t> selected=s.corners;if(!s.uvFocus&&polygonSelection_.object==s.object&&polygonSelection_.mode==PolygonSelectionMode::Face){selected.clear();for(auto f:polygonSelection_.components)for(unsigned j=0;j<3;++j)selected.insert(std::uint32_t(f*3+j));}
    for(std::uint32_t c=0;c<std::min<std::size_t>(mesh.indices.size(),60000);++c) {
        const auto other=c/3*3+(c+1)%3;
        const auto a=screen_uv(texcoord(mesh.vertices[mesh.indices[c]],s.channel),s,viewport);
        const auto b=screen_uv(texcoord(mesh.vertices[mesh.indices[other]],s.channel),s,viewport);
        const auto x=s.uvGeometric[mesh.indices[c]],y=s.uvGeometric[mesh.indices[other]];
        const auto edge=(std::uint64_t(std::min(x,y))<<32U)|std::max(x,y);
        const bool seam=s.uvFlagsValid && s.uvFlags.enabled && s.uvFlags.seams[s.channel].contains(edge);
        // A seam must remain identifiable even when that edge is selected.
        const auto tint=seam?color(255,100,90):selected.contains(c)?color(255,175,55):color(105,210,190);
        painter.line(int(std::clamp(a.x,-100000.0F,100000.0F)),int(std::clamp(a.y,-100000.0F,100000.0F)),
            int(std::clamp(b.x,-100000.0F,100000.0F)),int(std::clamp(b.y,-100000.0F,100000.0F)),
            tint,seam?3:selected.contains(c)?2:1);
        if(s.uvMode==0)painter.fill({int(std::clamp(a.x,-100000.0F,100000.0F))-3,
            int(std::clamp(a.y,-100000.0F,100000.0F))-3,7,7},
            s.uvFlagsValid && s.uvFlags.enabled && s.uvFlags.pins[s.channel].contains(c)?color(230,110,245):tint);
    }
    // Pins remain visible in Vertex, Edge, Face and Island modes. Otherwise an
    // automatic layout can refuse because of hidden pins the artist cannot see.
    if(s.uvFlagsValid && s.uvFlags.enabled) for(const auto corner : s.uvFlags.pins[s.channel]) {
        if(corner>=mesh.indices.size() || corner>=60000)continue;
        const auto point=screen_uv(texcoord(mesh.vertices[mesh.indices[corner]],s.channel),s,viewport);
        const int px=int(std::clamp(point.x,-100000.0F,100000.0F));
        const int py=int(std::clamp(point.y,-100000.0F,100000.0F));
        painter.fill({px-5,py-5,11,11},color(20,27,36));
        painter.fill({px-3,py-3,7,7},color(230,110,245));
    }
    if(s.selecting)painter.outline({std::min(s.startX,s.lastX),std::min(s.startY,s.lastY),std::abs(s.lastX-s.startX)+1,std::abs(s.lastY-s.startY)+1},color(255,190,65));
    canvas.text(panel.x+12,panel.y+panel.height-50,
        "Red: UV seams ("+std::to_string(s.uvFlagsValid?s.uvFlags.seams[s.channel].size():0)+
        ")  Purple: saved pins ("+std::to_string(s.uvFlagsValid?s.uvFlags.pins[s.channel].size():0)+
        ") | UV"+std::to_string(s.channel),color(215,195,240));
    canvas.text(panel.x+12,panel.y+panel.height-28,"Target "+std::to_string(s.uvDensityTarget)+" texels/m | Shift+B capture | C/Shift+C straighten",color(205,220,235));canvas.text(panel.x+12,panel.y+panel.height-8,s.typed.empty()?(s.uvFlagsValid?"P saved pin | M seam / Shift+M clear | J stitch | Esc close":"Saved UV flags need Reset UV Flags (or Undo topology edit)"):("Value: "+s.typed+" | Enter apply | Esc cancel"),color(255,205,145));
}
} // namespace dve::editor
