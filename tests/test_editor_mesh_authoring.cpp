#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/runtime_scene.hpp"
#include "dve/collision_hull.hpp"
#include "dve/polygon_collision.hpp"
#include "dve/physics_jolt_backend.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace dve;using namespace dve::editor;
namespace {
void check(bool x,const char* message){if(!x)throw std::runtime_error(message);}
void near(double a,double b,const char* message){check(std::abs(a-b)<1e-4,message);}
double volume(const CookedPolygonAsset& m){double out=0;for(std::size_t i=0;i<m.indices.size();i+=3){auto a=m.vertices[m.indices[i]].position,b=m.vertices[m.indices[i+1]].position,c=m.vertices[m.indices[i+2]].position;out+=double(a.x)*(double(b.y)*c.z-double(b.z)*c.y)+double(a.y)*(double(b.z)*c.x-double(b.x)*c.z)+double(a.z)*(double(b.x)*c.y-double(b.y)*c.x);}return out/6;}
EditorObject cube(EditorObjectId id){EditorObject o(id,"Cube");o.polygon=make_editor_polygon_cube(id);return o;}
void booleans(){
    EditorDocument d("Boolean");d.add_object(cube(1));auto b=cube(2);b.transform.position={.5F,0,0};b.polygon->materials[0].name="B";d.add_object(std::move(b));
    const auto hash=d.find_object(1)->polygon->contentHash;
    for(auto op:{MeshBoolean::Union,MeshBoolean::Difference,MeshBoolean::ReverseDifference,MeshBoolean::Intersection}){
        auto r=boolean_editor_polygons(d,1,2,op);if(!r)throw std::runtime_error(r.error);near(volume(r.mesh),op==MeshBoolean::Union?1.5:.5,"Boolean volume");check(d.find_object(1)->polygon->contentHash==hash,"Boolean backend is read-only");
        auto diagnostics=diagnose_polygon_topology(r.mesh);check(!diagnostics.boundaries&&!diagnostics.nonmanifold&&!diagnostics.degenerate,"Boolean output topology");check(r.mesh.materials.size()==2&&r.mesh.materials[1].name=="B","Boolean palette transfer");
        for(const auto& s:r.mesh.submeshes)check(s.materialIndex<2,"Boolean material provenance");for(const auto& v:r.mesh.vertices)check(std::isfinite(v.texcoord.x)&&std::isfinite(v.texcoord.y),"Interpolated finite UVs");}
    auto open=cube(3);open.polygon=make_editor_polygon_plane(3);d.add_object(std::move(open));check(!boolean_editor_polygons(d,1,3,MeshBoolean::Union),"Open Boolean input refused");d.find_object(2)->transform.position={3,0,0};check(!boolean_editor_polygons(d,1,2,MeshBoolean::Intersection),"Empty result refused");
    d.find_object(2)->transform.position={0,0,0};d.find_object(2)->voxelScale={.5F,1,1};auto intersection=boolean_editor_polygons(d,1,2,MeshBoolean::Intersection);check(bool(intersection),"Nonuniform metric Boolean");near(volume(intersection.mesh),.5,"metric volume");
}
void topology(){
    auto cubeMesh=make_editor_polygon_cube(1);check(diagnose_polygon_topology(cubeMesh).boundaries==0,"Seam-aware closed cube");PolygonSelection faces{1,PolygonSelectionMode::Face,{0}};auto subdivided=edit_polygon_topology(cubeMesh,faces,TopologyEdit::Subdivide);if(!subdivided)throw std::runtime_error(subdivided.error);check(!diagnose_polygon_topology(subdivided.mesh).boundaries,"Subdivision repairs adjacent triangles; no T junctions");near(volume(subdivided.mesh),1,"Subdivision volume");
    auto beveled=bevel_convex_editor_edges(cubeMesh,{1,PolygonSelectionMode::Edge,{1}},.1F);if(!beveled)throw std::runtime_error(beveled.error);near(volume(beveled.mesh),.99,"convex edge chamfer volume");check(!diagnose_polygon_topology(beveled.mesh).boundaries,"bevel remains closed");
    auto plane=make_editor_polygon_plane(1);PolygonSelection diagonal{1,PolygonSelectionMode::Edge,{(std::uint64_t(0)<<32U)|2U}};auto flipped=edit_polygon_topology(plane,diagonal,TopologyEdit::FlipDiagonal);if(!flipped)throw std::runtime_error(flipped.error);check(flipped.mesh.indices!=plane.indices,"Diagonal changes");
    auto missing=edit_polygon(cubeMesh,{1,PolygonSelectionMode::Face,{0,1}},PolygonEdit::Delete);check(bool(missing),"remove cube side");auto diagnostics=diagnose_polygon_topology(missing.mesh);check(diagnostics.boundaries==4,"one boundary loop");
    auto capped=edit_polygon_topology(missing.mesh,{1,PolygonSelectionMode::Edge,diagnostics.problemEdges},TopologyEdit::CapBoundaries);if(!capped)throw std::runtime_error(capped.error);check(!diagnose_polygon_topology(capped.mesh).boundaries,"cap closes loop");near(volume(capped.mesh),1,"Cap volume and winding");
    EditorDocument bridgeDocument("Bridge");EditorObject bottom(1,"Bottom");bottom.polygon=make_editor_polygon_plane(1,2);
    for(std::size_t i=0;i<bottom.polygon->indices.size();i+=3)std::swap(bottom.polygon->indices[i+1],bottom.polygon->indices[i+2]);refresh_editor_polygon(*bottom.polygon,true);
    EditorObject top(2,"Top");top.polygon=make_editor_polygon_plane(2,2);top.transform.position={0,1,0};bridgeDocument.add_object(std::move(bottom));bridgeDocument.add_object(std::move(top));
    const std::array<EditorObjectId,2> ids{1,2};auto pair=join_editor_polygons(bridgeDocument,1,ids);auto edges=diagnose_polygon_topology(pair.mesh);
    auto bridged=edit_polygon_topology(pair.mesh,{1,PolygonSelectionMode::Edge,edges.problemEdges},TopologyEdit::BridgeLoops);if(!bridged)throw std::runtime_error(bridged.error);check(!diagnose_polygon_topology(bridged.mesh).boundaries,"Bridge closes both loops");near(volume(bridged.mesh),4,"Bridge winding and volume");
    auto normals=edit_polygon_topology(cubeMesh,{1,PolygonSelectionMode::Face,polygon_all_components(cubeMesh,PolygonSelectionMode::Face)},TopologyEdit::SmoothNormals);check(bool(normals),"smooth normals");check(normals.mesh.indices.size()==cubeMesh.indices.size(),"normal operation keeps topology");
}
void uv(){
    auto m=make_editor_polygon_plane(1);std::set<std::uint32_t> all{0,1,2,3,4,5};check(polygon_uv_islands(m).size()==1,"plane UV island");UvEditSettings settings;settings.vector={.25F,-.5F};auto moved=edit_polygon_uv(m,{0,1,2},UvEdit::Translate,settings);check(bool(moved),"face UV move");near(moved.mesh.vertices[moved.mesh.indices[3]].texcoord.x,m.vertices[m.indices[3]].texcoord.x,"unselected face UV fixed");check(moved.mesh.vertices[moved.mesh.indices[0]].position.x==m.vertices[m.indices[0]].position.x,"UV leaves geometry fixed");
    auto unwrapped=edit_polygon_uv(m,all,UvEdit::Unwrap);if(!unwrapped)throw std::runtime_error(unwrapped.error);auto packed=edit_polygon_uv(unwrapped.mesh,all,UvEdit::Pack);check(bool(packed),"unwrap and pack");for(auto i:packed.mesh.indices){auto p=packed.mesh.vertices[i].texcoord;check(p.x>=.019F&&p.x<=.981F&&p.y>=.019F&&p.y<=.981F,"padding and unit bounds");}
    settings.pins={{0,{.2F,.3F}},{1,{.7F,.3F}}};auto pinned=edit_polygon_uv(m,all,UvEdit::Unwrap,settings);if(!pinned)throw std::runtime_error(pinned.error);near(pinned.mesh.vertices[pinned.mesh.indices[0]].texcoord.x,.2,"UV pin U");near(pinned.mesh.vertices[pinned.mesh.indices[1]].texcoord.x,.7,"second UV pin U");settings.pins.clear();
    settings.channel=1;settings.vector={2,2};auto scaled=edit_polygon_uv(m,all,UvEdit::Scale,settings);check(bool(scaled),"UV1 scale");near(scaled.mesh.vertices[0].texcoord.x,m.vertices[0].texcoord.x,"UV0 unchanged");settings.vector={0,1};check(!edit_polygon_uv(m,all,UvEdit::Scale,settings),"zero UV scale refused");
}
void uv_completion_density_and_alignment() {
    auto m=make_editor_polygon_plane(1,4);
    const std::set<std::uint32_t> all{0,1,2,3,4,5};
    UvEditSettings settings;settings.objectScale={2,1,1};
    const auto original=measure_polygon_uv_density(m,all,settings);
    check(bool(original) && original.charts.size()==1,"density measures a complete chart");
    check(original.geometricAreaSquareMeters>0 && original.uvArea>0,"density has physical and UV area");
    check(!measure_polygon_uv_density(m,{0,1,2},settings),"partial island density is rejected");
    settings.targetTexelsPerMeter=float(original.texelsPerMeter*2);
    const auto matched=edit_polygon_uv(m,all,UvEdit::MatchDensity,settings);
    check(bool(matched),matched.error.empty()?"density match succeeds":matched.error.c_str());
    if(matched){
        const auto changed=measure_polygon_uv_density(matched.mesh,all,settings);
        check(bool(changed),"matched island can be measured");
        if(changed)near(changed.texelsPerMeter,original.texelsPerMeter*2,"doubling target density");
        near(matched.mesh.vertices[matched.mesh.indices[0]].position.x,m.vertices[m.indices[0]].position.x,"positions stay unchanged");
        near(matched.mesh.vertices[matched.mesh.indices[0]].texcoord1.x,m.vertices[m.indices[0]].texcoord1.x,"UV1 unchanged");
        const auto horizontal=edit_polygon_uv(matched.mesh,all,UvEdit::AlignIslandU,settings);
        const auto vertical=edit_polygon_uv(matched.mesh,all,UvEdit::AlignIslandV,settings);
        check(bool(horizontal)&&bool(vertical),"island alignment on U or V");
        if(horizontal&&vertical){
            auto uh=measure_polygon_uv_density(horizontal.mesh,all,settings),uv=measure_polygon_uv_density(vertical.mesh,all,settings);
            if(uh&&uv){near(uh.uvArea,original.uvArea*4,"align U preserves chart area");near(uv.uvArea,original.uvArea*4,"align V preserves chart area");}
        }
    }
    settings.pins={{0,m.vertices[m.indices[0]].texcoord}};
    check(!edit_polygon_uv(m,all,UvEdit::MatchDensity,settings),"automatic density respects saved pins");
    settings.pins.clear();
    const auto straight=edit_polygon_uv(m,{0,1,2},UvEdit::StraightenU,settings);
    check(bool(straight),"selected UV corners can be straightened");
    if(straight){
        const auto& a=straight.mesh;
        const float y=a.vertices[a.indices[0]].texcoord.y;
        near(a.vertices[a.indices[1]].texcoord.y,y,"straighten horizontal 1");
        near(a.vertices[a.indices[2]].texcoord.y,y,"straighten horizontal 2");
        near(a.vertices[a.indices[3]].texcoord.y,m.vertices[m.indices[3]].texcoord.y,"unselected UV stays unchanged");
    }
    settings.channel=1;
    // Generated primitives leave UV1 all-zero (a collapsed chart), which must be refused.
    check(!measure_polygon_uv_density(m,all,settings),"collapsed UV1 refused");
    auto halfUv1=m;
    for(auto& vertex:halfUv1.vertices)vertex.texcoord1={vertex.texcoord.x*.5F,vertex.texcoord.y*.5F};
    refresh_editor_polygon(halfUv1,false);
    const auto uv1=measure_polygon_uv_density(halfUv1,all,settings);
    check(bool(uv1),"UV1 measured independently");
    if(uv1)near(uv1.texelsPerMeter,original.texelsPerMeter*.5,"UV1 density is independent of UV0");
    settings.textureWidth=0;
    check(!measure_polygon_uv_density(m,all,settings),"invalid texture size rejected");
}
void hull(){
    std::vector<Float3> points{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};check(validate_collision_hull_points(points),"valid tetrahedron points");auto encoded=encode_collision_hull_points(points);auto decoded=decode_collision_hull_points(encoded);check(decoded&&decoded->size()==4,"hull codec");check(!decode_collision_hull_points(encoded+"junk"),"trailing data refused");auto mesh=convex_editor_hull(1,points);check(bool(mesh),"convex hull");near(volume(mesh.mesh),1.0/6,"hull tetrahedron volume");points[3]={1,1,0};check(!validate_collision_hull_points(points)&&!convex_editor_hull(1,points),"flat hull refused");
    auto source=make_editor_polygon_cube(1);points=*decoded;PolygonCollisionOptions options;options.hullPoints=points;Float3 center;auto desc=make_polygon_dynamic_body_desc(source,{},options,&center);check(desc&&desc->convexHullPoints.size()==4,"hull descriptor");auto stat=make_polygon_static_body_desc(source,{},points);check(stat&&stat->convexHullPoints.size()==4,"static hull descriptor");
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());std::string error;auto id=world.spawn_cooked_polygon_asset(source,"Mesh",{},false,true,&error);check(id!=kInvalidGameObjectId,"runtime mesh");Component component;component.id=100;component.type=std::string(kCollisionHullComponent);component.properties["points"]=encoded;
    auto directory=std::filesystem::temp_directory_path()/("dve-hull-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(directory);
    EditorDocument document("Hull persistence");auto authored=cube(1);authored.voxelScale={2,.5F,3};authored.components.push_back(component);document.add_object(std::move(authored));
    auto saved=document.save_transactional(directory/"scene.dvescene");check(saved.success,"Hull scene save");auto loaded=EditorDocument::load(saved.manifestPath,&error);check(bool(loaded),"Hull scene reload");auto restored=find_component_by_type(std::span<const Component>(loaded->find_object(1)->components),kCollisionHullComponent);check(restored&&collision_hull_component_points(*restored),"Hull points retained after reload");
    auto exported=export_editor_scene(*loaded,EditorMaterialLibrary::make_default(),directory/"runtime.dvoxscene.json");if(!exported.success)throw std::runtime_error(exported.error);
    std::ifstream input(directory/"runtime.dvoxscene.json");std::string json((std::istreambuf_iterator<char>(input)),{});auto manifest=parse_dvoxscene_manifest(json);check(bool(manifest),"Exported hull manifest parses");
    auto baked=find_component_by_type(std::span<const Component>(manifest->objects[0].components),kCollisionHullComponent);check(baked!=nullptr,"Hull component exported");auto bakedPoints=collision_hull_component_points(*baked);check(bool(bakedPoints),"Baked hull points valid");near((*bakedPoints)[1].x,2,"Export hull X metric");near((*bakedPoints)[2].y,.5,"Export hull Y metric");near((*bakedPoints)[3].z,3,"Export hull Z metric");input.close();std::error_code cleanupError;std::filesystem::remove_all(directory,cleanupError);
    check(world.add_component(id,component,&error)!=nullptr,"runtime applies authored hull");check(!world.set_component_property(id,100,"points",std::string("invalid"),&error),"runtime refuses invalid hull edit");check(std::get<std::string>(world.component(id,100)->properties.at("points"))==std::get<std::string>(component.properties.at("points")),"invalid hull update retains metadata");check(world.set_component_enabled(id,100,false),"runtime disables hull");check(!world.set_component_property(id,100,"points",std::string("invalid"),&error),"disabled hull still validates points");check(world.set_component_enabled(id,100,true),"runtime reenables hull");check(world.remove_component(id,100),"runtime restores default collision");
}
struct Canvas final:IEditorCanvas{mutable unsigned lines{},fills{};void fill(UiRect,EditorColor)const override{++fills;}void outline(UiRect,EditorColor)const override{}void line(int,int,int,int,EditorColor,int)const override{++lines;}void text(int,int,std::string_view,EditorColor)const override{}int text_width(std::string_view s)const override{return int(s.size()*8);}};

std::string first_line(const std::filesystem::path& p){std::ifstream in(p);std::string line;std::getline(in,line);return line;}
std::uint64_t edge_key(std::uint32_t a,std::uint32_t b){return (std::uint64_t(std::min(a,b))<<32U)|std::max(a,b);}
void uv_flags_persistence(){
    EditorDocument document("UV flags");document.add_object(cube(1));
    auto* object=document.find_object(1);std::string error;
    auto empty=read_polygon_uv_authoring(*object,&error);check(empty&&empty->enabled&&empty->seams[0].empty()&&empty->pins[1].empty(),"no record reads as empty flags");
    PolygonUvAuthoring flags=*empty;const auto edges=polygon_all_components(*object->polygon,PolygonSelectionMode::Edge);
    flags.seams[0].insert(*edges.begin());flags.seams[1].insert(*edges.rbegin());flags.pins[0]={0,5};flags.pins[1]={7};
    auto command=make_polygon_uv_authoring_command(document,*object,flags,&error);check(bool(command),"UV flag command builds");
    EditorCommandStack stack;check(stack.execute(document,std::move(command)).success,"UV flag command executes");
    auto directory=std::filesystem::temp_directory_path()/"dve_uv_flags_roundtrip";std::filesystem::remove_all(directory);
    auto saved=document.save_transactional(directory/"scene.dvescene");check(saved.success,"UV flag scene saves");
    check(first_line(saved.manifestPath)=="DVE_EDITOR_SCENE 7","polygon scene is format 7");
    auto loaded=EditorDocument::load(saved.manifestPath,&error);check(bool(loaded),"UV flag scene reloads");
    auto restored=read_polygon_uv_authoring(*loaded->find_object(1),&error);
    check(restored&&restored->seams==flags.seams&&restored->pins==flags.pins&&restored->enabled,"UV seams and pins survive save/load per channel");
    auto& bad=*loaded->find_object(1);for(auto& c:bad.components)if(c.type==kUvAuthoringComponent)c.properties["state"]=std::string("DVE_UV 1 0 36\n0 0\n0 0\n");
    check(!read_polygon_uv_authoring(bad,&error)&&!error.empty(),"stale layout hash refused with reason");
    EditorDocument plain("No polygons");plain.add_object(EditorObject(1,"Empty"));
    auto plainSaved=plain.save_transactional(directory/"plain.dvescene");check(plainSaved.success,"plain scene saves");
    check(first_line(plainSaved.manifestPath)=="DVE_EDITOR_SCENE 6","scenes without polygons stay format 6");
    check(bool(EditorDocument::load(plainSaved.manifestPath,&error)),"format 6 scene reloads");
    std::filesystem::remove_all(directory);
}
void seam_unwrap(){
    auto plane=make_editor_polygon_plane(1);const std::set<std::uint32_t> all{0,1,2,3,4,5};
    check(polygon_uv_islands(plane).size()==1,"plane is one island without seams");
    const auto geometric=polygon_geometric_vertices(plane);
    UvEditSettings settings;settings.seams={edge_key(geometric[plane.indices[0]],geometric[plane.indices[2]])};
    check(polygon_uv_islands(plane,0,settings.seams).size()==2,"diagonal seam splits the island");
    auto cut=edit_polygon_uv(plane,all,UvEdit::Unwrap,settings);if(!cut)throw std::runtime_error(cut.error);
    check(cut.mesh.vertices.size()>plane.vertices.size(),"seam unwrap duplicates seam vertices");
    check(polygon_uv_islands(cut.mesh,0,settings.seams).size()==2,"seam unwrap keeps separate UV islands");check(cut.mesh.indices[0]!=cut.mesh.indices[3]||cut.mesh.indices[2]!=cut.mesh.indices[4],"seam corners no longer share vertex attributes");
    check(polygon_uv_layout_hash(cut.mesh)==polygon_uv_layout_hash(plane),"seam cut keeps the saved-flag layout hash");
    settings.seams={edge_key(1000,1001)};check(!edit_polygon_uv(plane,all,UvEdit::Unwrap,settings),"stale seam refused");
    auto closed=make_editor_polygon_cube(1);for(auto& v:closed.vertices)v.texcoord={v.position.x*.3F+v.position.z*.1F,v.position.y*.3F+v.position.z*.05F};
    std::set<std::uint32_t> cubeAll;for(std::uint32_t c=0;c<closed.indices.size();++c)cubeAll.insert(c);
    refresh_editor_polygon(closed,false);check(polygon_uv_islands(closed).size()==1,"welded cube is one closed island");
    check(bool(edit_polygon_uv(closed,cubeAll,UvEdit::Unwrap)),"closed chart without seams falls back per triangle");
    const auto cg=polygon_geometric_vertices(closed);settings.seams={edge_key(cg[closed.indices[0]],cg[closed.indices[1]])};
    auto failed=edit_polygon_uv(closed,cubeAll,UvEdit::Unwrap,settings);
    check(!failed&&failed.error.find("disk boundary")!=std::string::npos,"seamed chart that cannot flatten fails honestly");
}
const MenuAction& menu(NativeEditorController& c,std::string_view id){c.refresh_menu_state();const auto& menus=c.workspace().menus();const auto* a=menus.find(id);if(!a)throw std::runtime_error("missing menu");return *a;}
bool has_flags(NativeEditorController& c){return find_component_by_type(std::span<const Component>(c.workspace().document().find_object(1)->components),kUvAuthoringComponent)!=nullptr;}
void uv_flag_controller(){
    EditorDocument d("UV flag gating");d.add_object(cube(1));d.add_object(cube(2));
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(d)));auto& c=*owner;c.resize(1600,900);c.workspace().select_object(1);
    check(c.set_polygon_mode(PolygonSelectionMode::Edge),"edge mode");
    const auto edges=polygon_all_components(*c.workspace().document().find_object(1)->polygon,PolygonSelectionMode::Edge);
    const std::set<std::uint64_t> picked{*edges.begin()};c.select_polygon_components(picked,SelectionOperation::Replace);
    check(menu(c,"mesh.uv_mark_seam").enabled,"mark seam enabled with edges");
    check(!menu(c,"mesh.uv_reset_flags").enabled&&!menu(c,"mesh.uv_reset_flags").disabledReason.empty(),"reset disabled with reason when no flags");
    check(!c.dispatch_action("mesh.uv_reset_flags")&&!has_flags(c),"reset without flags refuses");
    auto refused=[&](const char* why){auto& m=menu(c,"mesh.uv_mark_seam");check(!m.enabled&&!m.disabledReason.empty(),why);check(!c.dispatch_action("mesh.uv_mark_seam")&&!has_flags(c),why);};
    auto* o=c.workspace().document().find_object(1);
    o->flags.locked=true;refused("locked mesh refuses seam edits");o->flags.locked=false;
    o->flags.visible=false;refused("hidden mesh refuses seam edits");o->flags.visible=true;
    c.workspace().select_object(2);check(c.dispatch_action("view.isolate_selection"),"isolate other");c.workspace().select_object(1);
    check(c.is_isolated_out(1),"object isolated out");refused("isolated-out mesh refuses seam edits");
    c.workspace().select_object(2);(void)c.dispatch_action("view.isolate_selection");check(c.isolated_objects().empty(),"isolation cleared");
    c.workspace().select_object(1);
    if(c.dispatch_action("physics.play")){check(c.play_session().active(),"play active");refused("Play refuses seam edits");check(c.dispatch_action("physics.stop"),"stop play");}
    else throw std::runtime_error("physics.play did not start");
    check(c.set_polygon_mode(PolygonSelectionMode::Edge),"edge mode again");c.select_polygon_components(picked,SelectionOperation::Replace);
    // Mark seam: one undo step, selection restored on undo.
    check(c.dispatch_action("mesh.uv_mark_seam")&&has_flags(c),"mark seam saves flags");
    c.select_polygon_components({},SelectionOperation::Replace);
    check(c.dispatch_action("edit.undo")&&!has_flags(c),"mark seam undoes in one step");
    check(c.polygon_selection().components==picked,"mark seam undo restores selection");
    check(c.dispatch_action("edit.redo")&&has_flags(c),"mark seam redo");
    c.select_polygon_components(picked,SelectionOperation::Replace);
    check(c.dispatch_action("mesh.uv_clear_seam"),"clear seam");
    check(read_polygon_uv_authoring(*c.workspace().document().find_object(1))->seams[0].empty(),"seam cleared");
    c.select_polygon_components({},SelectionOperation::Replace);
    check(c.dispatch_action("edit.undo")&&read_polygon_uv_authoring(*c.workspace().document().find_object(1))->seams[0]==picked,"clear seam undo");
    check(c.polygon_selection().components==picked,"clear seam undo restores selection");
    // Pins in the UV workspace.
    check(c.dispatch_action("mesh.uv_editor"),"UV workspace opens");c.key_down("a",false,false,false);
    const auto faces=c.polygon_selection().components;check(!faces.empty(),"UV select all");
    check(menu(c,"mesh.uv_toggle_pin").enabled,"toggle pin enabled");
    check(c.dispatch_action("mesh.uv_toggle_pin"),"toggle pins");
    check(read_polygon_uv_authoring(*c.workspace().document().find_object(1))->pins[0].size()==36,"all corners pinned");
    c.select_polygon_components({},SelectionOperation::Replace);
    check(c.dispatch_action("edit.undo")&&read_polygon_uv_authoring(*c.workspace().document().find_object(1))->pins[0].empty(),"pin undo one step");
    check(c.polygon_selection().components==faces,"pin undo restores selection");
    // Another preview open refuses flag edits.
    c.key_down("a",false,false,false);c.key_down("g",false,false,false);c.key_down("1",false,false,false);check(c.mesh_authoring_dragging(),"UV preview open");
    check(!menu(c,"mesh.uv_toggle_pin").enabled&&!menu(c,"mesh.uv_toggle_pin").disabledReason.empty(),"pin disabled during preview");
    check(!menu(c,"mesh.uv_reset_flags").enabled,"reset disabled during preview");
    check(!c.dispatch_action("mesh.uv_toggle_pin"),"pin refused during preview");
    c.key_down("escape",false,false,false);check(!c.mesh_authoring_dragging(),"preview cancelled");
    // Reset.
    check(menu(c,"mesh.uv_reset_flags").enabled,"reset enabled with flags");
    const auto before=c.polygon_selection().components;
    check(c.dispatch_action("mesh.uv_reset_flags")&&!has_flags(c),"reset removes flags");
    c.select_polygon_components({},SelectionOperation::Replace);
    check(c.dispatch_action("edit.undo")&&has_flags(c),"reset undo one step");
    check(c.polygon_selection().components==before,"reset undo restores selection");
}
void native(){
    EditorDocument d("Native authoring");d.add_object(cube(1));auto b=cube(2);b.transform.position={.5F,0,0};d.add_object(std::move(b));auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(d)));auto& c=*owner;c.resize(1600,900);c.workspace().select_object(2);c.workspace().add_to_selection(1);
    check(c.dispatch_action("mesh.boolean_difference"),"native Boolean preview");check(c.workspace().document().objects().size()==2,"preview preserves inputs");c.key_down("enter",false,false,false);check(c.workspace().document().objects().size()==3,"Boolean new output");check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==2,"Boolean Undo");check(c.workspace().selection_count()==2,"Boolean undo selection");
    c.workspace().select_object(1);auto geometry=c.workspace().document().find_object(1)->polygon->contentHash;check(c.dispatch_action("mesh.hull_fit"),"native fit hull");check(find_component_by_type(std::span<const Component>(c.workspace().document().find_object(1)->components),kCollisionHullComponent),"hull saved as component");c.key_down("x",false,false,false);c.key_down("minus",false,false,false);c.key_down("1",false,false,false);c.key_down("enter",false,false,false);check(c.workspace().document().find_object(1)->polygon->contentHash==geometry,"hull edits preserve visible geometry");check(c.dispatch_action("edit.undo"),"Hull point undo");c.key_down("escape",false,false,false);
    check(c.dispatch_action("mesh.uv_editor"),"UV workspace opens");c.key_down("a",false,false,false);c.key_down("g",false,false,false);c.key_down("1",false,false,false);check(c.mesh_authoring_dragging(),"typed UV preview");check(!c.dispatch_action("file.save"),"unapplied UVs cannot save");c.key_down("enter",false,false,false);check(c.workspace().document().find_object(1)->polygon->contentHash!=geometry,"UV edit applies");check(c.dispatch_action("edit.undo")&&c.workspace().document().find_object(1)->polygon->contentHash==geometry,"UV Undo exact");
    // UV-Completion-1: density measure, typed target, preview, cancel and one undo.
    c.key_down("q",false,false,false);
    c.key_down("a",false,false,false);
    c.key_down("b",false,false,false);
    check(c.status().text.find("texels/m")!=std::string::npos,"density measurement visible to artist");
    c.key_down("i",false,false,false);
    for(const char* digit:{"2","5","6"})c.key_down(digit,false,false,false);
    c.key_down("enter",false,false,false);
    check(c.status().text.find("256")!=std::string::npos,"numeric density target accepted");
    c.key_down("n",false,false,false);
    check(c.mesh_authoring_dragging(),"density matching opens a cancellable preview");
    c.key_down("escape",false,false,false);
    check(c.workspace().document().find_object(1)->polygon->contentHash==geometry,
          "density cancel restores source mesh byte-equivalent content hash");
    c.key_down("n",false,false,false);
    check(c.mesh_authoring_dragging(),"density preview opens again");
    c.key_down("enter",false,false,false);
    check(c.workspace().document().find_object(1)->polygon->contentHash!=geometry,
          "density match commits changed UV coordinates");
    check(c.dispatch_action("edit.undo")&&
          c.workspace().document().find_object(1)->polygon->contentHash==geometry,
          "density edit is one undoable command");
    c.key_down("o",false,false,false);
    check(c.mesh_authoring_dragging(),"UV island alignment previews");
    c.key_down("escape",false,false,false);
    check(c.workspace().document().find_object(1)->polygon->contentHash==geometry,
          "island alignment cancellation restores original UVs");
    Canvas canvas;c.draw_mesh_authoring(canvas,c.layout().viewport);check(canvas.lines&&canvas.fills,"UV canvas draws geometry and checker");
}
}
void bevel_options(){
    auto cubeMesh=make_editor_polygon_cube(1);PolygonSelection edge{1,PolygonSelectionMode::Edge,{1}};
    const auto sound=[](const PolygonEditResult& r,const char* what){if(!r)throw std::runtime_error(std::string(what)+": "+r.error);auto t=diagnose_polygon_topology(r.mesh);
        check(!t.boundaries&&!t.nonmanifold&&!t.degenerate&&!t.inconsistentWinding,"bevel option output is a closed manifold");check(r.selection.mode==PolygonSelectionMode::Face&&!r.selection.components.empty(),"bevel selects new faces");
        for(const auto& v:r.mesh.vertices){check(std::abs(length(v.normal)-1)<1e-3F,"unit bevel normals");check(std::isfinite(v.texcoord.x)&&std::isfinite(v.texcoord.y)&&std::isfinite(v.texcoord1.x),"finite bevel UVs");}};
    EdgeBevelSettings chamfer;chamfer.depth=0;chamfer.segments=1;auto flat=bevel_convex_editor_edges(cubeMesh,edge,chamfer);sound(flat,"chamfer");
    EdgeBevelSettings round;round.depth=1;round.segments=4;round.smoothingAngle=60;auto fillet=bevel_convex_editor_edges(cubeMesh,edge,round);sound(fillet,"fillet");
    const double vc=volume(flat.mesh),vf=volume(fillet.mesh);check(vc<1&&vf<1&&vc>.9,"bevel removes material");check(vf>vc,"rounded fillet removes less than flat chamfer");
    check(fillet.mesh.indices.size()>flat.mesh.indices.size(),"segments add profile faces");
    // Smoothing angle 60 smooths the 22.5-degree fillet steps: some corner normal is not a face normal.
    bool smoothed=false;for(std::size_t i=0;i<fillet.mesh.indices.size()&&!smoothed;i+=3){auto a=fillet.mesh.vertices[fillet.mesh.indices[i]],b=fillet.mesh.vertices[fillet.mesh.indices[i+1]],c=fillet.mesh.vertices[fillet.mesh.indices[i+2]];
        auto x=subtract(b.position,a.position),y=subtract(c.position,a.position);auto n=normalize(Float3{x.y*y.z-x.z*y.y,x.z*y.x-x.x*y.z,x.x*y.y-x.y*y.x});smoothed=dot(n,a.normal)<.999F;}
    check(smoothed,"fillet smoothing angle produces shared normals");
    EdgeBevelSettings bad=round;bad.segments=0;check(!bevel_convex_editor_edges(cubeMesh,edge,bad),"zero segments refused");bad=round;bad.width=1.5F;check(!bevel_convex_editor_edges(cubeMesh,edge,bad),"fraction above one refused");
    bad=round;bad.width=0;check(!bevel_convex_editor_edges(cubeMesh,edge,bad),"zero width refused");bad=round;bad.depth=2;check(!bevel_convex_editor_edges(cubeMesh,edge,bad),"depth above one refused");
    EdgeBevelSettings tiny;tiny.offsetType=BevelOffsetType::Absolute;tiny.width=1e-9F;tiny.autoFit=false;auto none=bevel_convex_editor_edges(cubeMesh,edge,tiny);check(!none||!none.selection.components.empty(),"zero-width bevel is refused, never a no-op success");
    EdgeBevelSettings world;world.offsetType=BevelOffsetType::Absolute;world.width=.1F;world.depth=0;world.autoFit=false;auto scaled=bevel_convex_editor_edges(cubeMesh,edge,world,{2,2,2});sound(scaled,"world-space bevel");
    world.worldSpace=false;auto local=bevel_convex_editor_edges(cubeMesh,edge,world,{2,2,2});sound(local,"local bevel");check(volume(scaled.mesh)>volume(local.mesh),"world offset scales with object size");
    world.worldSpace=true;check(!bevel_convex_editor_edges(cubeMesh,edge,world,{0,1,1}),"zero object scale refused");
    EditorDocument d("Concave");d.add_object(cube(1));auto b=cube(2);b.transform.position={.5F,.5F,0};d.add_object(std::move(b));auto ell=boolean_editor_polygons(d,1,2,MeshBoolean::Union);check(bool(ell),"L solid");
    bool refusedConcave=false;for(auto key:polygon_all_components(ell.mesh,PolygonSelectionMode::Edge)){auto r=bevel_convex_editor_edges(ell.mesh,{1,PolygonSelectionMode::Edge,{key}},round);check(!r,"concave solid bevel refused");if(r.error.find("convex")!=std::string::npos)refusedConcave=true;}
    check(refusedConcave,"concave refusal names the convex limitation");
}
int main(){try{uv_flags_persistence();seam_unwrap();uv_flag_controller();uv();uv_completion_density_and_alignment();if(!mesh_boolean_available()){std::cout<<"Manifold authoring disabled in this build\n";return 0;}booleans();topology();bevel_options();hull();native();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Mesh authoring regression checks passed\n";}
