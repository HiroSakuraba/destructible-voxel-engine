#include "dve/editor_polygon.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/editor_play_session.hpp"
#include "dve/editor_scatter.hpp"
#include "dve/voxel_metric.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace dve;
using namespace dve::editor;
namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void near(float a,float b,const char* message) { check(std::abs(a-b)<0.0001F,message); }
EditorObject mesh_object(EditorObjectId id,bool plane=false) {
    EditorObject object(id,plane?"Surface":"Mesh");object.voxelSizeMeters=1;
    object.polygon=plane?make_editor_polygon_plane(id):make_editor_polygon_cube(id);
    object.flags.anchored=true;return object;
}
float volume(const CookedPolygonAsset& mesh) {
    double sum=0;
    for(std::size_t i=0;i<mesh.indices.size();i+=3) {
        const auto a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position;
        const auto c=mesh.vertices[mesh.indices[i+2]].position;
        const Float3 cross{b.y*c.z-b.z*c.y,b.z*c.x-b.x*c.z,b.x*c.y-b.y*c.x};
        sum+=dot(a,cross)/6.0;
    }
    return float(sum);
}
void geometry() {
    const auto cube=make_editor_polygon_cube(1);check(bool(validate_polygon_asset(cube)),"cube asset");
    check(polygon_all_components(cube,PolygonSelectionMode::Vertex).size()==8,"geometric vertex identity across seams");
    check(polygon_edges(cube).size()==18,"cube physical edges and six face diagonals");
    PolygonSelection vertex{1,PolygonSelectionMode::Vertex,{0}};PolygonEditSettings settings;settings.vector={.25F,0,0};
    const auto moved=edit_polygon(cube,vertex,PolygonEdit::Translate,settings);check(bool(moved),"vertex move");
    for(std::size_t i=0;i<cube.vertices.size();++i)
        if(length_squared(cube.vertices[i].position)==0) near(moved.mesh.vertices[i].position.x,.25F,"split seam follows vertex move");
    const auto plane=make_editor_polygon_plane(2);PolygonSelection faces{2,PolygonSelectionMode::Face,{0,1}};
    settings.amount=1;const auto extruded=edit_polygon(plane,faces,PolygonEdit::Extrude,settings);
    check(bool(extruded)&&extruded.mesh.indices.size()==30,"region extrusion uses outer boundary only");
    near(extruded.mesh.bounds.maximum.y,1,"extrude distance");
    settings.inset=.25F;const auto inset=edit_polygon(plane,faces,PolygonEdit::Inset,settings);
    check(bool(inset)&&inset.mesh.indices.size()==30,"inset ring");
    near(std::abs(inset.mesh.vertices[inset.mesh.indices[0]].position.x),1.5F,"inset fraction");
    const auto bevel=edit_polygon(plane,faces,PolygonEdit::Bevel,settings);
    check(bool(bevel),"face bevel");
    const auto front=slice_polygon(cube,{.5F,0,0},{1,0,0},true);
    const auto back=slice_polygon(cube,{.5F,0,0},{1,0,0},false);
    check(bool(front)&&bool(back),"capped closed-mesh slice");
    near(volume(front.mesh),.5F,"front closed volume");near(volume(back.mesh),.5F,"back closed volume");
    check(!slice_polygon(plane,{0,0,0},{1,0,0},true,true),"open contour refused for caps");
    check(bool(slice_polygon(plane,{0,0,0},{1,0,0},true,false)),"open surface cut without caps");
    settings.vector={1,0,1};
    check(!edit_polygon(cube,vertex,PolygonEdit::Scale,settings),"zero scale refused");

    EditorDocument document("Meshes");auto first=mesh_object(1);auto second=mesh_object(2);
    second.transform.position={3,0,0};second.voxelScale={2,.5F,1};
    document.add_object(std::move(first));document.add_object(std::move(second));
    const std::array<EditorObjectId,2> ids{1,2};const auto joined=join_editor_polygons(document,1,ids);
    check(bool(joined)&&joined.mesh.materials.size()==2,"join palettes");
    near(joined.mesh.bounds.maximum.x,5,"join world transforms and metrics");
    check(separate_polygon_components(joined.mesh).size()==2,"separate islands");

    const auto* object=document.find_object(2);const auto hit=pick_editor_polygon(*object,{{0,.25F,.5F},{1,0,0}},10);
    check(bool(hit),"polygon ray");near(hit->distance,3,"polygon ray distance in meters");
    const auto picked=pick_editor_document(document,{{-2,.5F,.5F},{1,0,0}});
    check(picked&&picked->polygonFace,"shared viewport picking includes triangle identity");
    EditorCamera camera;camera.position={.5F,.5F,5};camera.target={.5F,.5F,.5F};
    const auto all=polygon_components_in_rect(*document.find_object(1),camera,{0,0,800,600},{0,0,800,600},
        PolygonSelectionMode::Vertex,true);
    check(all.size()==8,"through rectangle component selection");
}
void history_and_storage() {
    EditorDocument document("Polygon storage");document.add_object(mesh_object(1));
    const auto before=*document.find_object(1)->polygon;PolygonEditSettings settings;settings.vector={.2F,0,0};
    PolygonSelection selection{1,PolygonSelectionMode::Vertex,polygon_all_components(before,PolygonSelectionMode::Vertex)};
    auto changed=edit_polygon(before,selection,PolygonEdit::Translate,settings);check(bool(changed),"edit for history");
    const auto hash=changed.mesh.contentHash;EditorCommandStack stack;
    check(stack.execute(document,std::make_unique<ReplacePolygonCommand>(1,before,changed.mesh,"Move mesh")).success,"mesh commit");
    for(int i=0;i<3;++i) {
        check(stack.undo(document).success&&document.find_object(1)->polygon->contentHash==before.contentHash,"mesh undo");
        check(stack.redo(document).success&&document.find_object(1)->polygon->contentHash==hash,"mesh redo");
    }
    document.find_object(1)->flags.locked=true;
    check(!stack.execute(document,std::make_unique<ReplacePolygonCommand>(1,changed.mesh,before,"Locked")).success,"locked mesh guard");
    document.find_object(1)->flags.locked=false;
    document.find_object(1)->voxelScale={2,.5F,3};
    document.find_object(1)->sourceAsset="missing-source.dmesh";
    const auto directory=std::filesystem::temp_directory_path()/
        ("dve-polygon-editor-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto saved=document.save_transactional(directory/"scene.dvescene");check(saved.success,saved.error.c_str());
    std::string error;auto loaded=EditorDocument::load(saved.manifestPath,&error);check(bool(loaded),error.c_str());
    check(loaded->find_object(1)->polygon&&loaded->find_object(1)->polygon->contentHash==hash,"owned mesh scene round trip");
    near(loaded->find_object(1)->voxelScale.z,3,"scene mesh scale");
    const auto cloned=clone_editor_document(*loaded);check(cloned.find_object(1)->polygon->contentHash==hash,"clone owns mesh");
    const auto exported=export_editor_scene(*loaded,EditorMaterialLibrary::make_default(),directory/"runtime.dvoxscene.json");
    check(exported.success,exported.error.c_str());
    const auto runtime=read_dmesh(directory/"runtime.objects"/"1.dmesh");check(bool(runtime),runtime.error.c_str());
    near(runtime.asset.bounds.maximum.z,3,"export bakes scale from edited mesh without reading missing source");
    EditorWorkspace workspace(std::move(*loaded));EditorPlaySession session;EditorPlaySessionConfig config;
    config.preferProductionPhysics=false;
    check(session.start(workspace,EditorMaterialLibrary::make_default(),EditorMode::Simulate,config,{},&error),error.c_str());
    (void)session.stop(workspace,&error);
    std::filesystem::remove_all(directory);
}
void shared_workflows() {
    EditorDocument document("Mixed scatter");auto source=mesh_object(1);source.transform.position={5,0,0};
    document.add_object(std::move(source));document.add_object(mesh_object(2,true));
    ScatterSettings settings;settings.count=4;settings.minSpacingMeters=.2F;settings.minScale=.5F;settings.maxScale=1.5F;
    const auto plan=plan_scatter(document,2,1,settings);check(plan.error.empty()&&plan.samples.size()==4,"scatter on polygon terrain");
    const auto repeated=plan_scatter(document,2,1,settings);
    for(std::size_t i=0;i<plan.samples.size();++i)
        check(length(subtract(plan.samples[i].position,repeated.samples[i].position))==0,"deterministic polygon scatter");
    const auto group=document.allocate_object_id();auto command=std::make_unique<CompoundCommand>("Scatter meshes");
    command->add(std::make_unique<AddObjectCommand>(make_scatter_group(group,plan.samples.size(),{},settings),"Scatter group"));
    const std::vector<ScatterSource> sources{{&document,{1},"Mesh"}};
    const auto copies=append_scatter_copies(*command,document,group,plan.samples,sources,false);
    check(copies.error.empty()&&copies.objectCount==4,"mesh scatter copies");
    EditorCommandStack stack;check(stack.execute(document,std::move(command)).success,"scatter history");
    for(auto id:copies.rootIds) {
        const auto* object=document.find_object(id);
        check(object&&object->polygon&&object->polygon->objectId==id,"scatter deep copy mesh identity");
        check(object->voxelScale.x>=.5F&&object->voxelScale.x<=1.5F,"scatter geometry scale");
    }
    check(stack.undo(document).success&&document.objects().size()==2,"scatter undo");

    NativeEditorController controller{EditorWorkspace(std::move(document))};
    controller.resize(1600,900);controller.workspace().select_object(1);controller.workspace().add_to_selection(2);
    check(controller.dispatch_action("edit.group"),"group polygon objects");
    check(controller.workspace().document().find_object(1)->polygon.has_value(),"group preserves polygon data");
    check(controller.dispatch_action("edit.undo"),"group undo");
    check(controller.dispatch_action("create.polygon"),"native polygon cube creation");
    const auto id=controller.workspace().selected_object();
    check(id&&controller.workspace().document().find_object(*id)->polygon,"native owned mesh");
    check(!controller.polygon_draw_items().empty(),"native polygon preview geometry");
}
}
int main() {
    try {geometry();history_and_storage();shared_workflows();std::cout<<"Polygon editor checks passed\n";}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
