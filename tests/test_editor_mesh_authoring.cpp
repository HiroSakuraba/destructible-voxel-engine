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
void hull(){
    std::vector<Float3> points{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};check(validate_collision_hull_points(points),"valid tetrahedron points");auto encoded=encode_collision_hull_points(points);auto decoded=decode_collision_hull_points(encoded);check(decoded&&decoded->size()==4,"hull codec");check(!decode_collision_hull_points(encoded+"junk"),"trailing data refused");auto mesh=convex_editor_hull(1,points);check(bool(mesh),"convex hull");near(volume(mesh.mesh),1.0/6,"hull tetrahedron volume");points[3]={1,1,0};check(!validate_collision_hull_points(points)&&!convex_editor_hull(1,points),"flat hull refused");
    auto source=make_editor_polygon_cube(1);points=*decoded;PolygonCollisionOptions options;options.hullPoints=points;Float3 center;auto desc=make_polygon_dynamic_body_desc(source,{},options,&center);check(desc&&desc->convexHullPoints.size()==4,"hull descriptor");auto stat=make_polygon_static_body_desc(source,{},points);check(stat&&stat->convexHullPoints.size()==4,"static hull descriptor");
    GameWorld world(std::make_unique<ReferenceRigidBodyWorld>());std::string error;auto id=world.spawn_cooked_polygon_asset(source,"Mesh",{},false,true,&error);check(id!=kInvalidGameObjectId,"runtime mesh");Component component;component.id=100;component.type=std::string(kCollisionHullComponent);component.properties["points"]=encoded;
    auto directory=std::filesystem::temp_directory_path()/("dve-hull-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(directory);
    EditorDocument document("Hull persistence");auto authored=cube(1);authored.voxelScale={2,.5F,3};authored.components.push_back(component);document.add_object(std::move(authored));
    auto saved=document.save_transactional(directory/"scene.dvescene");check(saved.success,"Hull scene save");auto loaded=EditorDocument::load(saved.manifestPath,&error);check(bool(loaded),"Hull scene reload");auto restored=find_component_by_type(std::span<const Component>(loaded->find_object(1)->components),kCollisionHullComponent);check(restored&&collision_hull_component_points(*restored),"Hull points retained after reload");
    auto exported=export_editor_scene(*loaded,EditorMaterialLibrary::make_default(),directory/"runtime.dvoxscene.json");if(!exported.success)throw std::runtime_error(exported.error);
    std::ifstream input(directory/"runtime.dvoxscene.json");std::string json((std::istreambuf_iterator<char>(input)),{});auto manifest=parse_dvoxscene_manifest(json);check(bool(manifest),"Exported hull manifest parses");
    auto baked=find_component_by_type(std::span<const Component>(manifest->objects[0].components),kCollisionHullComponent);check(baked!=nullptr,"Hull component exported");auto bakedPoints=collision_hull_component_points(*baked);check(bool(bakedPoints),"Baked hull points valid");near((*bakedPoints)[1].x,2,"Export hull X metric");near((*bakedPoints)[2].y,.5,"Export hull Y metric");near((*bakedPoints)[3].z,3,"Export hull Z metric");std::filesystem::remove_all(directory);
    check(world.add_component(id,component,&error)!=nullptr,"runtime applies authored hull");check(!world.set_component_property(id,100,"points",std::string("invalid"),&error),"runtime refuses invalid hull edit");check(std::get<std::string>(world.component(id,100)->properties.at("points"))==std::get<std::string>(component.properties.at("points")),"invalid hull update retains metadata");check(world.set_component_enabled(id,100,false),"runtime disables hull");check(!world.set_component_property(id,100,"points",std::string("invalid"),&error),"disabled hull still validates points");check(world.set_component_enabled(id,100,true),"runtime reenables hull");check(world.remove_component(id,100),"runtime restores default collision");
}
struct Canvas final:IEditorCanvas{mutable unsigned lines{},fills{};void fill(UiRect,EditorColor)const override{++fills;}void outline(UiRect,EditorColor)const override{}void line(int,int,int,int,EditorColor,int)const override{++lines;}void text(int,int,std::string_view,EditorColor)const override{}int text_width(std::string_view s)const override{return int(s.size()*8);}};
void native(){
    EditorDocument d("Native authoring");d.add_object(cube(1));auto b=cube(2);b.transform.position={.5F,0,0};d.add_object(std::move(b));auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(d)));auto& c=*owner;c.resize(1600,900);c.workspace().select_object(2);c.workspace().add_to_selection(1);
    check(c.dispatch_action("mesh.boolean_difference"),"native Boolean preview");check(c.workspace().document().objects().size()==2,"preview preserves inputs");c.key_down("enter",false,false,false);check(c.workspace().document().objects().size()==3,"Boolean new output");check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==2,"Boolean Undo");check(c.workspace().selection_count()==2,"Boolean undo selection");
    c.workspace().select_object(1);auto geometry=c.workspace().document().find_object(1)->polygon->contentHash;check(c.dispatch_action("mesh.hull_fit"),"native fit hull");check(find_component_by_type(std::span<const Component>(c.workspace().document().find_object(1)->components),kCollisionHullComponent),"hull saved as component");c.key_down("x",false,false,false);c.key_down("minus",false,false,false);c.key_down("1",false,false,false);c.key_down("enter",false,false,false);check(c.workspace().document().find_object(1)->polygon->contentHash==geometry,"hull edits preserve visible geometry");check(c.dispatch_action("edit.undo"),"Hull point undo");c.key_down("escape",false,false,false);
    check(c.dispatch_action("mesh.uv_editor"),"UV workspace opens");c.key_down("a",false,false,false);c.key_down("g",false,false,false);c.key_down("1",false,false,false);check(c.mesh_authoring_dragging(),"typed UV preview");check(!c.dispatch_action("file.save"),"unapplied UVs cannot save");c.key_down("enter",false,false,false);check(c.workspace().document().find_object(1)->polygon->contentHash!=geometry,"UV edit applies");check(c.dispatch_action("edit.undo")&&c.workspace().document().find_object(1)->polygon->contentHash==geometry,"UV Undo exact");Canvas canvas;c.draw_mesh_authoring(canvas,c.layout().viewport);check(canvas.lines&&canvas.fills,"UV canvas draws geometry and checker");
}
}
int main(){try{if(!mesh_boolean_available()){std::cout<<"Manifold authoring disabled in this build\n";return 0;}booleans();topology();uv();hull();native();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Mesh authoring regression checks passed\n";}
