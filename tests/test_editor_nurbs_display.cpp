#include "dve/editor_native.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool b,const std::string& e){if(!b)throw std::runtime_error(e);}
void near(double a,double b,double t=2e-6){check(std::abs(a-b)<t,"numeric mismatch "+std::to_string(a)+" / "+std::to_string(b));}
constexpr auto legacy="DVE_NURBS 1 3 2 2 1 5 0 0 0.5 1 1 1 4 0 0 1 1 0 0 0 1 1 0 0 1 1 1 0 1 0 0 1 1 1 0 1 1 1 1 1 1 ";
NurbsSource folded(){auto s=decode_nurbs(legacy);check(bool(s),"legacy decode");s->creaseNormals=true;return *s;}
Float3 cross3(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
void face_normals(const CookedPolygonAsset& mesh){
    for(std::size_t i=0;i<mesh.indices.size();i+=3){const auto& a=mesh.vertices[mesh.indices[i]];const auto& b=mesh.vertices[mesh.indices[i+1]];const auto& c=mesh.vertices[mesh.indices[i+2]];auto n=normalize(cross3(subtract(b.position,a.position),subtract(c.position,a.position)));for(auto index:{mesh.indices[i],mesh.indices[i+1],mesh.indices[i+2]})check(dot(n,mesh.vertices[index].normal)>.999F,"each planar face uses its own crease normal");}
}
void geometry(){
    std::string error;auto old=decode_nurbs(legacy,&error);check(bool(old),error);check(!old->creaseNormals,"v1 retains legacy display");
    auto mesh=tessellate_nurbs(*old,42,&error);check(bool(mesh),error);
    // Captured with the parent commit's v1 header/library, not the new encoder.
    check(mesh->contentHash==15216819910113250660ULL&&mesh->vertices.size()==15&&mesh->indices.size()==48,"legacy bake stays byte-compatible");
    check(decode_nurbs(encode_nurbs(*old))==old,"v1 to v2 preserves legacy flag");
    auto s=folded();mesh=tessellate_nurbs(s,42,&error);check(bool(mesh),error);check(mesh->vertices.size()==18&&mesh->indices.size()==48,"duplicate crease vertices without bridge triangles");face_normals(*mesh);
    unsigned seam=0;for(const auto& v:mesh->vertices)if(std::abs(v.position.x-1)<1e-6&&std::abs(v.position.y)<1e-6&&std::abs(v.position.z-.5)<1e-6){++seam;near(v.texcoord.x,.5);near(v.texcoord.y,.5);}check(seam==2,"coincident seam vertices share UVs");
    auto transposed=transpose_nurbs(s);check(bool(transposed),"transpose");mesh=tessellate_nurbs(*transposed,43,&error);check(bool(mesh),error);face_normals(*mesh);
    NurbsSource both;both.columns=both.rows=3;both.samplesPerSpan=2;both.u={1,{0,0,.5,1,1}};both.v=both.u;
    for(unsigned j=0;j<3;++j)for(unsigned i=0;i<3;++i)both.controls.push_back({double(i),double((i==2)+(j==2)),double(j),1});
    mesh=tessellate_nurbs(both,44,&error);check(bool(mesh),error);check(mesh->vertices.size()==36&&mesh->indices.size()==96,"intersecting creases duplicate four corner normals");face_normals(*mesh);
    auto dense=s;dense.samplesPerSpan=8;check(dense.controls==s.controls,"density retains source net");auto finer=tessellate_nurbs(dense,45,&error);check(bool(finer),error);check(finer->vertices.size()>18&&finer->indices.size()>48,"density changes display mesh");
    for(double u:{.1,.3,.7,.9}){auto a=evaluate_nurbs(s,u,.3),b=evaluate_nurbs(dense,u,.3);check(a&&b,"density evaluation");for(unsigned i=0;i<3;++i)near(a->position[i],b->position[i],1e-12);}
    auto curved=extrude_nurbs(make_nurbs_arc(),{0,2,0});check(bool(curved),"smooth conic seam fixture");mesh=tessellate_nurbs(*curved,46,&error);check(bool(mesh),error);
    std::vector<Float3> seamNormals;for(const auto& v:mesh->vertices)if(std::abs(v.texcoord.x-.25)<1e-6&&std::abs(v.texcoord.y-.5)<1e-6)seamNormals.push_back(v.normal);check(seamNormals.size()==2&&dot(seamNormals[0],seamNormals[1])>.99999F,"geometrically smooth conic seam stays smooth");
    s.textureU={2,5};s.textureV={-1,3};check(decode_nurbs(encode_nurbs(s))==s,"display metadata codec");
    for(bool v:{false,true}){
        auto halves=split_nurbs(s,v,.37,&error);check(bool(halves),error);
        for(const auto* half:{&halves->first,&halves->second}){auto m=tessellate_nurbs(*half,47,&error);check(bool(m),error);for(const auto& point:m->vertices){double u=std::abs(point.position.y)<1e-8?.5*point.position.x:.5+.5*point.position.y;double w=point.position.z;near(point.texcoord.x,2+3*u);near(point.texcoord.y,-1+4*w);near(point.texcoord1.x,point.texcoord.x);near(point.texcoord1.y,point.texcoord.y);}}
        auto splitAgain=split_nurbs(halves->first,v,.18,&error);check(bool(splitAgain),error);const auto range=v?splitAgain->second.textureV:splitAgain->second.textureU;near(range[0],v?-.28:2.54);near(range[1],v?.48:3.11);
    }
    auto swapped=transpose_nurbs(s);check(swapped&&swapped->textureU==s.textureV&&swapped->textureV==s.textureU,"texture domains follow swapped axes");
    auto bad=s;bad.textureU={1,1};check(!validate_nurbs(bad),"empty texture range refuses");bad=s;bad.textureV[1]=std::numeric_limits<double>::infinity();check(!validate_nurbs(bad),"nonfinite texture refuses");
    check(!decode_nurbs("DVE_NURBS 2 3 2 2 9 0 1 0 1"),"invalid display flag refuses");
    auto cap=make_nurbs_patch();cap.columns=cap.rows=129; // Invalid source controls must fail before display allocation.
    check(!validate_nurbs(cap),"source budget refuses");
    // At raw grid 256x256, crease duplicates would push the actual mesh over cap.
    NurbsSource grid;grid.u={1,{0,0}};grid.v={1,{0,0}};
    // Use 64x64 controls with 4 samples/span: raw 253x253, split grid 315x315.
    grid.columns=grid.rows=64;grid.samplesPerSpan=4;
    for(unsigned i=1;i<63;++i){grid.u.knots.push_back(double(i));grid.v.knots.push_back(double(i));}grid.u.knots.insert(grid.u.knots.end(),2,63);grid.v.knots.insert(grid.v.knots.end(),2,63);
    for(unsigned j=0;j<64;++j)for(unsigned i=0;i<64;++i)grid.controls.push_back({double(i),0,double(j),1});
    grid.creaseNormals=false;check(validate_nurbs(grid),"legacy raw budget valid");grid.creaseNormals=true;check(!validate_nurbs(grid),"duplicate vertex budget refuses");
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("NURBS display")));auto& c=*owner;c.resize(1600,1100);
    auto old=decode_nurbs(legacy);auto object=make_editor_nurbs(1,*old);check(bool(object),"make legacy object");find_component_by_type(std::span<Component>(object->components),kNurbsComponent)->properties["source"]=std::string(legacy);
    c.workspace().document().add_object(std::move(*object));c.workspace().select_object(1);check(c.workspace().document().validate(),"legacy source/cache validates");
    auto* o=c.workspace().document().find_object(1);auto hash=o->polygon->contentHash;
    check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({1,2},SelectionOperation::Replace),"select for upgrade");
    check(c.dispatch_action("nurbs.source_quality"),"upgrade legacy crease display");check(editor_nurbs_source(*o)->creaseNormals&&o->polygon->contentHash!=hash,"upgrade source and cache together");check(c.selected_nurbs_controls()==std::set<std::size_t>{1,2},"quality preserves CV selection");
    check(c.dispatch_action("edit.undo")&&!editor_nurbs_source(*o)->creaseNormals&&o->polygon->contentHash==hash,"undo restores legacy bake");check(c.dispatch_action("edit.redo"),"redo upgrade");
    auto history=c.workspace().commands().size();check(c.dispatch_action("nurbs.source_quality")&&c.workspace().commands().size()==history,"no-op quality has no history");
    check(c.dispatch_action("nurbs.construct_settings"),"quality settings");auto v=c.layout().viewport;c.pointer_down(PointerButton::Primary,v.x+25,v.y+8+125+4*32+12,0);check(c.text_edit().axis==8,"density field opens");c.text_input("5");c.key_down("enter",false,false,false);check(c.dispatch_action("nurbs.source_quality")&&editor_nurbs_source(*o)->samplesPerSpan==5,"density field applies");
    check(c.dispatch_action("edit.undo")&&editor_nurbs_source(*o)->samplesPerSpan==8,"undo density");
    {   // Codex P2: opening the density panel from control mode keeps the CV selection for Apply.
        check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({2,3},SelectionOperation::Replace),"select before settings");
        check(c.dispatch_action("nurbs.construct_settings"),"settings from control mode");
        c.pointer_down(PointerButton::Primary,v.x+25,v.y+8+125+4*32+12,0);c.text_input("6");c.key_down("enter",false,false,false);
        check(c.dispatch_action("nurbs.source_quality")&&editor_nurbs_source(*o)->samplesPerSpan==6,"density via panel");
        check(c.selected_nurbs_controls()==std::set<std::size_t>{2,3},"settings panel preserved CV selection");
        check(c.dispatch_action("edit.undo")&&editor_nurbs_source(*o)->samplesPerSpan==5,"undo panel density");
        check(c.dispatch_action("edit.redo")&&c.dispatch_action("edit.undo"),"redo/undo panel density");
    }check(c.dispatch_action("edit.redo"),"redo density");
    check(c.dispatch_action("nurbs.source_split_u"),"native texture-preserving split");auto id=*c.workspace().selected_object();auto* half=c.workspace().document().find_object(id);auto s=editor_nurbs_source(*half);check(s&&s->textureU==std::array<double,2>{0,.5},"native split stores texture subrange");
    check(c.dispatch_action("nurbs.source_reset_uv")&&editor_nurbs_source(*half)->textureU==std::array<double,2>{0,1},"explicit texture reset");check(c.dispatch_action("edit.undo")&&editor_nurbs_source(*half)==s,"undo UV reset");
    auto savedUv=half->polygon->vertices[0].texcoord;half->polygon->vertices[0].texcoord.x+=.1F;check(!validate_editor_nurbs(*half),"source protects cached UVs");half->polygon->vertices[0].texcoord=savedUv;
    auto savedNormal=half->polygon->vertices[0].normal;half->polygon->vertices[0].normal.x+=.1F;check(!validate_editor_nurbs(*half),"source protects crease normals");half->polygon->vertices[0].normal=savedNormal;
    const auto root=std::filesystem::temp_directory_path()/"dve_nurbs_display_regression";std::filesystem::remove_all(root);std::filesystem::create_directories(root);std::string error;
    const auto save=c.workspace().document().save_transactional(root/"scene.dvescene");check(save.success,save.error);auto loaded=EditorDocument::load(root/"scene.dvescene",&error);check(bool(loaded),error);check(editor_nurbs_source(*loaded->find_object(id))==s,"reopened texture subrange and density");check(loaded->find_object(id)->polygon->contentHash==half->polygon->contentHash,"reopened crease-aware bake");std::filesystem::remove_all(root);
    c.workspace().select_object(1);check(c.dispatch_action("nurbs.construct_settings"),"reopen settings");c.pointer_down(PointerButton::Primary,v.x+25,v.y+8+125+4*32+12,0);c.text_input("65");c.key_down("enter",false,false,false);check(!c.text_edit().error.empty(),"invalid density reports error");c.key_down("escape",false,false,false);check(c.workspace().document().validate(),"final scene validates");
}
}
int main(){try{geometry();native();std::cout<<"NURBS crease/UV/display tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
