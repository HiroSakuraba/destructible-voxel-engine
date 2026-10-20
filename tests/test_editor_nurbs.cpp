#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_nurbs.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/game_scene_loader.hpp"
#include "dve/physics3d_backend.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool ok,const std::string& s){if(!ok)throw std::runtime_error(s);}
void close_to(double a,double b,double tol=1e-9){check(std::abs(a-b)<tol,"numeric mismatch "+std::to_string(a)+" / "+std::to_string(b));}
void math(){
    std::string e;auto arc=make_nurbs_arc(2,1.5707963267948966);
    check(validate_nurbs(arc,&e),e);
    auto q=evaluate_nurbs(arc,.5,0,&e);check(bool(q),e);
    close_to(q->position[0],std::sqrt(2.0));close_to(q->position[2],std::sqrt(2.0));
    close_to(q->du[0]*q->position[0]+q->du[2]*q->position[2],0);
    auto l=evaluate_nurbs(arc,.5-1e-6),r=evaluate_nurbs(arc,.5+1e-6);
    close_to((r->position[0]-l->position[0])/2e-6,q->du[0],1e-6);
    for(auto& p:arc.controls)p.weight*=1e50;
    auto scaled=evaluate_nurbs(arc,.5);check(bool(scaled),"global weight scaling");
    close_to(scaled->position[0],q->position[0]);
    auto full=make_nurbs_arc();for(double u:{0.0,.25,.5,.75,1.0}){
        auto p=evaluate_nurbs(full,u);check(bool(p),"repeated-knot circle sample");close_to(std::hypot(p->position[0],p->position[2]),1);
    }
    auto patch=make_nurbs_patch(4);auto c=evaluate_nurbs(patch,.25,.75);
    close_to(c->position[0],-1);close_to(c->position[2],-1);close_to(c->du[0],4);close_to(c->dv[2],-4);
    auto ex=extrude_nurbs(full,{0,3,0},&e);check(bool(ex),e);
    auto p=evaluate_nurbs(*ex,.25,.5);close_to(p->position[1],1.5);close_to(std::hypot(p->position[0],p->position[2]),1);
    NurbsSource line;line.columns=2;line.u={1,{0,0,1,1}};line.controls={{2,0,0,1},{2,4,0,1}};
    auto rev=revolve_nurbs(line);check(bool(rev),"revolve source");
    auto rp=evaluate_nurbs(*rev,.5,.125);close_to(rp->position[1],2);close_to(std::hypot(rp->position[0],rp->position[2]),2);
    auto codec=encode_nurbs(*rev);auto decoded=decode_nurbs(codec,&e);check(decoded&&*decoded==*rev,"codec roundtrip");
    check(!decode_nurbs(codec+" junk"),"trailing data rejected");
    check(!decode_nurbs("DVE_NURBS 1 4294967295 4294967295 8"),"allocation bound");
    check(!evaluate_nurbs(patch,-.1,.5),"outside domain");
    auto bad=patch;bad.controls[0].weight=0;check(!validate_nurbs(bad),"zero weight");
    bad=patch;bad.controls[0].weight=std::numeric_limits<double>::infinity();check(!validate_nurbs(bad),"infinite weight");
    bad=patch;bad.u.knots[4]=-1;check(!validate_nurbs(bad),"unsorted knot");
    bad=patch;bad.controls.resize(2);check(!validate_nurbs(bad),"bad net");
    bad=patch;bad.samplesPerSpan=65;check(!validate_nurbs(bad),"sample cap");
    // Independent affine reproduction fixtures for every supported degree.
    for(unsigned degree=1;degree<=7;++degree){
        NurbsSource b;b.columns=degree+1;b.u.degree=degree;b.u.knots.assign(degree+1,0);b.u.knots.insert(b.u.knots.end(),degree+1,1);
        for(unsigned i=0;i<=degree;++i)b.controls.push_back({3*double(i)/degree,2,0,1});
        auto at=evaluate_nurbs(b,.37);check(bool(at),"degree fixture");close_to(at->position[0],1.11);close_to(at->du[0],3);
    }
    NurbsSource nonuniform;nonuniform.columns=4;nonuniform.u={2,{0,0,0,.3,1,1,1}};
    nonuniform.controls={{0,0,0,1},{1,2,0,.7},{3,1,0,2},{4,0,0,1}};
    auto a=evaluate_nurbs(nonuniform,.6),b=evaluate_nurbs(nonuniform,.6-1e-6),c2=evaluate_nurbs(nonuniform,.6+1e-6);
    check(a&&b&&c2,"nonuniform rational fixture");close_to(a->du[0],(c2->position[0]-b->position[0])/2e-6,1e-6);
    auto mesh=tessellate_nurbs(patch,7,&e);check(mesh&&validate_polygon_asset(*mesh),"valid patch bake: "+e);
    close_to(mesh->vertices.front().normal.y,1,1e-6);
    check(tessellate_nurbs(full,8).has_value(),"curve ribbon");
    {   // Ribbon offset must stay continuous where the tangent crosses |t.y| = 0.9 (Codex P2).
        NurbsSource steep;steep.columns=3;steep.u={2,{0,0,0,1,1,1}};steep.controls={{0,0,0,1},{1,0,0,1},{1,3,0,1}};
        auto rm=tessellate_nurbs(steep,10,&e);check(bool(rm),"steep curve ribbon: "+e);
        const std::size_t n=rm->vertices.size()/2;
        for(std::size_t i=1;i<n;++i){auto o0=subtract(rm->vertices[n+i-1].position,rm->vertices[i-1].position),o1=subtract(rm->vertices[n+i].position,rm->vertices[i].position);
            const float c=(o0.x*o1.x+o0.y*o1.y+o0.z*o1.z)/(length(o0)*length(o1));check(c>.8F,"ribbon frame jumped between samples");}
    }
    check(tessellate_nurbs(*rev,9).has_value(),"revolution bake");
    bad=patch;for(auto& v:bad.controls)v.x=v.y=v.z=0;
    check(!tessellate_nurbs(bad,1),"singular patch rejected");
}
void commands_and_persistence(){
    std::string e;EditorDocument d("NURBS regression");auto source=make_nurbs_patch();
    auto o=make_editor_nurbs(1,source,&e);check(bool(o),e);d.add_object(std::move(*o));
    auto* object=d.find_object(1);check(object->is_nurbs()&&!object->is_polygon(),"geometry discriminator");
    auto hash=object->polygon->contentHash;auto after=source;after.controls[5].y=.8;after.controls[5].weight=2;
    EditNurbsCommand cmd(1,source,after);check(cmd.execute(d).success,"source edit");check(object->polygon->contentHash!=hash,"derived mesh regenerated");
    check(cmd.undo(d).success&&object->polygon->contentHash==hash,"undo exact geometry");
    check(cmd.execute(d).success,"redo");check(!cmd.execute(d).success,"stale command refuses");
    check(cmd.undo(d).success,"restore source");
    auto invalid=source;invalid.controls[2].weight=-1;EditNurbsCommand refused(1,source,invalid);
    check(!refused.execute(d).success&&object->polygon->contentHash==hash,"invalid edit atomic");
    auto changed=*object->polygon;changed.vertices[0].position.x+=1;
    ReplacePolygonCommand poly(1,*object->polygon,changed,"Forbidden mesh edit");
    check(!poly.execute(d).success&&object->polygon->contentHash==hash,"native source protected");
    check(!d.set_component_property(1,1,"source",std::string("bad"),&e),"generic source write blocked");
    check(!d.set_component_enabled(1,1,false),"source disable blocked");
    object->flags.locked=true;check(!cmd.execute(d).success,"locked edit");object->flags.locked=false;
    const auto root=std::filesystem::temp_directory_path()/"dve_nurbs_regression";
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    auto saved=d.save_transactional(root/"scene.dvescene");check(saved.success,saved.error);
    auto loaded=EditorDocument::load(root/"scene.dvescene",&e);check(bool(loaded),e);
    auto back=editor_nurbs_source(*loaded->find_object(1),&e);check(back&&*back==source,"reopened native source");
    check(loaded->find_object(1)->polygon->contentHash==hash,"reopened bake");
    auto clone=clone_editor_object(*object);check(editor_nurbs_source(clone)==back,"cloned source");
    object->polygon->vertices[0].position.x+=1;check(!d.validate(&e),"divergent cache detected");object->polygon->vertices[0].position.x-=1;
    auto baked=bake_editor_polygon(*object);check(bool(validate_polygon_asset(baked)),"runtime bake");
    const auto exported=export_editor_scene(d,EditorMaterialLibrary::make_default(),root/"runtime.dvoxscene.json");
    check(exported.success,exported.error);
    auto content=LooseContentSource::open(root,&e);check(bool(content),e);
    GameWorld world(create_physics3d_world(Physics3DBackend::Reference));
    auto runtime=load_scene_into_game_world(*content,"runtime.dvoxscene.json",world);check(bool(runtime),runtime.error.message);
    check(runtime.objects.size()==1&&runtime.objects[0].geometry==RuntimeSceneGeometry::Polygon,"packaged polygon load");
    const auto* components=world.components(runtime.objects[0].gameObjectId);check(components!=nullptr,"runtime components");
    check(find_component_by_type(*components,kNurbsComponent)==nullptr,"editor source stripped from runtime");
    std::filesystem::remove_all(root);
}
struct Canvas: IEditorCanvas {
    mutable std::string labels;
    void fill(UiRect,EditorColor)const override{}void outline(UiRect,EditorColor)const override{}
    void line(int,int,int,int,EditorColor,int)const override{}
    void text(int,int,std::string_view s,EditorColor)const override{labels+=std::string(s)+"\n";}
    int text_width(std::string_view s)const override{return int(s.size()*6);}
};
void native(){
    auto controller=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Native NURBS")));
    auto& c=*controller;c.resize(1600,1000);c.update(0);
    check(c.dispatch_action("nurbs.create_patch"),"native create patch");
    auto id=*c.workspace().selected_object();auto* o=c.workspace().document().find_object(id);
    check(o&&o->is_nurbs(),"native source");auto hash=o->polygon->contentHash;
    Canvas canvas;c.draw_nurbs(canvas,c.layout().viewport);check(canvas.labels.find("NURBS controls")!=std::string::npos,"draw control panel");
    // Numeric X field through the real pointer/text/Enter path.
    auto v=c.layout().viewport;c.pointer_down(PointerButton::Primary,v.x+30,v.y+8+62+12,0);c.text_input("-1.25");c.key_down("enter",false,false,false);
    auto s=editor_nurbs_source(*o);check(s&&s->controls[0].x==-1.25,"numeric CV field");
    check(c.dispatch_action("edit.undo"),"native undo control");close_to(editor_nurbs_source(*o)->controls[0].x,-2);
    check(c.dispatch_action("edit.redo"),"native redo control");
    // Invalid weight and cancel keep the source unchanged.
    auto prior=editor_nurbs_source(*o);
    c.pointer_down(PointerButton::Primary,v.x+30,v.y+8+62+3*32+12,0);c.text_input("0");c.key_down("enter",false,false,false);check(editor_nurbs_source(*o)==prior,"invalid weight untouched");c.key_down("escape",false,false,false);
    check(!c.dispatch_action("polygon.extrude"),"polygon edits refuse native source");
    {auto added=c.add_component_to_primary("dve.nurbs");check(!added.success,"generic empty NURBS source refused");check(c.workspace().document().validate(),"doc valid after refused add");}
    check(!c.dispatch_action("subdiv.from_polygon"),"subdivision conversion refuses native source");
    check(c.dispatch_action("nurbs.bake_polygon"),"native bake");
    auto bakedId=*c.workspace().selected_object();check(bakedId!=id&&c.workspace().document().find_object(bakedId)->is_polygon(),"baked output distinct");
    check(c.workspace().document().find_object(id)->is_nurbs(),"source retained");
    check(c.dispatch_action("edit.undo"),"undo bake");check(!c.workspace().document().find_object(bakedId),"bake removed");
    check(c.dispatch_action("edit.redo"),"redo bake");
    check(c.dispatch_action("nurbs.create_circle"),"create circle");
    auto curveId=*c.workspace().selected_object();
    c.workspace().document().find_object(curveId)->transform.position={3.0F,1.0F,-2.0F};
    const auto undoDepth=c.workspace().commands().size();
    check(c.dispatch_action("nurbs.extrude"),"extrude selected curve");
    check(c.workspace().commands().size()==undoDepth+1,"extrude is one undo step");
    auto extrudedId=*c.workspace().selected_object();
    check(c.workspace().document().find_object(extrudedId)->transform.position.x==3.0F,"extrusion inherits curve pose");
    check(c.dispatch_action("edit.undo")&&!c.workspace().document().find_object(extrudedId),"undo extrusion");
    check(c.workspace().selected_object()==curveId,"undo extrusion restores selection");
    check(c.dispatch_action("edit.redo"),"redo extrusion");
    check(c.workspace().document().find_object(extrudedId)&&c.workspace().document().find_object(extrudedId)->transform.position.x==3.0F,"redo keeps pose and id");
    check(c.workspace().document().find_object(curveId)->is_nurbs(),"curve retained");
    {const auto depth=c.workspace().commands().size();auto* cur=c.workspace().document().find_object(*c.workspace().selected_object());
     if(cur&&cur->is_nurbs()){check(c.dispatch_action("nurbs.edit"),"edit controls");auto q=editor_nurbs_source(*cur)->controls[0];
      check(c.edit_nurbs_control(0,q),"no-op control edit");check(c.workspace().commands().size()==depth,"no-op adds no undo step");}}
    check(c.dispatch_action("nurbs.create_profile"),"create profile");
    check(c.dispatch_action("nurbs.revolve"),"revolve selected profile");
    check(c.dispatch_action("nurbs.create_revolve"),"default revolve");
    auto count=c.workspace().document().objects().size();
    check(c.dispatch_action("physics.play"),"Play with native bakes");check(!c.dispatch_action("nurbs.create_patch"),"Play creation refuses");
    check(c.dispatch_action("physics.stop"),"stop Play");check(c.workspace().document().objects().size()==count,"Play retained authoring");
    check(c.workspace().document().validate(),"document valid after workflows");
    (void)hash;
}
}
int main(){try{math();commands_and_persistence();native();std::cout<<"Native NURBS tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

