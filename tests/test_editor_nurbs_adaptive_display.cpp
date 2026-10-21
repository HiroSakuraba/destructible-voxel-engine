#include "dve/editor_native.hpp"
#include "dve/nurbs_modeling.hpp"
#include <cmath>
#include <utility>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace dve;using namespace dve::editor;
namespace {
void check(bool b,const std::string& message){if(!b)throw std::runtime_error(message);}
constexpr auto legacy="DVE_NURBS 1 3 2 2 1 5 0 0 0.5 1 1 1 4 0 0 1 1 0 0 0 1 1 0 0 1 1 1 0 1 0 0 1 1 1 0 1 1 1 1 1 1 ";
void codec_geometry(){
    auto s=make_nurbs_patch(2);s.adaptiveDisplay=true;s.chordTolerance=.0025;s.normalAngle=.075;s.textureU={2,5};
    auto encoded=encode_nurbs(s);check(encoded.starts_with("DVE_NURBS 3 ")&&decode_nurbs(encoded)==s,"v3 adaptive roundtrip");
    auto old=decode_nurbs(legacy);check(old&&!old->adaptiveDisplay&&!old->creaseNormals,"v1 legacy default policy");check(decode_nurbs(encode_nurbs(*old))==old,"v1 upgrade preserves legacy display");
    auto fixed=make_nurbs_patch(2);std::istringstream in(encode_nurbs(fixed));std::vector<std::string> tokens;for(std::string t;in>>t;)tokens.push_back(t);tokens[1]="2";tokens.erase(tokens.begin()+10,tokens.begin()+13);std::string v2;for(auto t:tokens)v2+=t+' ';check(decode_nurbs(v2)==fixed,"v2 legacy default policy");
    std::string error;auto mesh=tessellate_nurbs(s,1,&error),direct=tessellate_nurbs_adaptive(s,1,s.chordTolerance,s.normalAngle,&error);check(mesh&&direct,error);check(mesh->contentHash==direct->contentHash&&mesh->vertices.size()==4,"Display dispatcher uses persisted adaptive policy");
    auto curved=s;curved.controls[5].y=.5;auto a=tessellate_nurbs(curved,1,&error);check(bool(a),error);check(a->vertices.size()>4,"Source changes drive adaptive refinement");
    auto split=split_nurbs(curved,false,.3,&error);check(bool(split),error);check(split->first.adaptiveDisplay&&split->first.chordTolerance==s.chordTolerance,"Splits retain adaptive policy");check(tessellate_nurbs(split->first,2,&error).has_value(),error);
    auto crop=crop_nurbs_surface(curved,.1,.9,.1,.9,&error);check(crop&&crop->adaptiveDisplay,"Crop retains adaptive policy");auto transpose=transpose_nurbs(curved);check(transpose&&transpose->adaptiveDisplay,"Transpose retains policy");auto elevated=elevate_nurbs_degree(curved,false);check(elevated&&elevated->adaptiveDisplay,"Elevation retains policy");
    auto edges=extract_nurbs_boundaries(curved,&error);check(bool(edges),error);for(auto& e:*edges)check(!e.adaptiveDisplay&&e.is_curve(),"Extracted curves use fixed ribbons");
    for(unsigned test=0;test<5;++test){auto bad=s;if(test==0)bad.chordTolerance=0;if(test==1)bad.chordTolerance=NAN;if(test==2)bad.normalAngle=0;if(test==3)bad.normalAngle=4;if(test==4)bad.normalAngle=INFINITY;check(!validate_nurbs(bad)&&encode_nurbs(bad).empty(),"Invalid display policy refuses");}
    auto curve=make_nurbs_arc();curve.adaptiveDisplay=true;check(!validate_nurbs(curve),"Curve adaptive display refuses");check(!decode_nurbs("DVE_NURBS 3 4 4 8 1 0 1 0 1 2 0.01 0.15"),"Invalid codec flag refuses");
    NurbsSource grid;grid.columns=grid.rows=64;grid.samplesPerSpan=64;grid.u={1,{0,0}};grid.v={1,{0,0}};for(unsigned i=1;i<63;++i){grid.u.knots.push_back(i);grid.v.knots.push_back(i);}grid.u.knots.insert(grid.u.knots.end(),2,63);grid.v.knots.insert(grid.v.knots.end(),2,63);for(unsigned j=0;j<64;++j)for(unsigned i=0;i<64;++i)grid.controls.push_back({double(i),0,double(j),1});check(!validate_nurbs(grid),"Fixed grid exceeds display budget");grid.adaptiveDisplay=true;check(validate_nurbs(grid),"Adaptive source independent of fixed density budget");check(tessellate_nurbs(grid,1,&error).has_value(),error);
    auto object=make_editor_nurbs(1,s,&error);check(bool(object),error);object->polygon->vertices[0].normal.x+=.1F;check(!validate_editor_nurbs(*object),"Adaptive stale normal refuses");
    s.creaseNormals=false;object=make_editor_nurbs(2,s,&error);check(bool(object),error);object->polygon->vertices[0].normal.x+=.1F;check(!validate_editor_nurbs(*object),"Adaptive normals validated even for a legacy crease flag");
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Adaptive display")));auto& c=*owner;c.resize(1600,1000);auto original=make_nurbs_patch(2);auto object=make_editor_nurbs(1,original);check(bool(object),"Native surface");object->transform.position={4,2,1};object->voxelScale={2,.5F,1};object->polygon->materials[0].name="Retained";object->polygon->contentHash=polygon_asset_content_hash(*object->polygon);c.workspace().document().add_object(std::move(*object));c.workspace().select_object(1);
    check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({5,6},SelectionOperation::Replace),"Control selection");auto* o=c.workspace().document().find_object(1);const auto fixedHash=o->polygon->contentHash;
    check(c.dispatch_action("nurbs.construct_settings"),"Park selection in settings");auto history=c.workspace().commands().size();check(c.dispatch_action("nurbs.source_display_standard"),"Enable adaptive display");auto source=editor_nurbs_source(*o);check(source&&source->adaptiveDisplay&&o->polygon->vertices.size()==4&&c.workspace().commands().size()==history+1&&c.selected_nurbs_controls()==std::set<std::size_t>{5,6},"Adaptive cache one command / selection");check(o->polygon->materials[0].name=="Retained"&&o->transform.position.x==4&&o->voxelScale.y==.5F,"Appearance/placement retained");
    const auto* menu=std::as_const(c.workspace().menus()).find("nurbs.source_display_standard");check(menu&&menu->checkable&&menu->checked,"Adaptive menu radio state");
    check(c.dispatch_action("edit.undo")&&editor_nurbs_source(*o)==original&&o->polygon->contentHash==fixedHash&&c.selected_nurbs_controls()==std::set<std::size_t>{5,6},"Undo source/cache/selection");check(c.dispatch_action("edit.redo")&&editor_nurbs_source(*o)==source,"Redo adaptive policy");
    auto point=source->controls[5];point.y=.25;check(c.edit_nurbs_control(5,point),"CV edit rebuilds adaptive display");check(o->polygon->vertices.size()>4&&editor_nurbs_source(*o)->adaptiveDisplay,"Adaptive cache responds to edit");check(c.dispatch_action("edit.undo")&&o->polygon->vertices.size()==4,"Undo control and adaptive cache");
    check(c.dispatch_action("nurbs.source_display_coarse"),"Coarse preset");check(editor_nurbs_source(*o)->chordTolerance==.05,"Coarse settings");check(c.dispatch_action("nurbs.source_display_fine"),"Fine preset");auto fine=editor_nurbs_source(*o);check(fine->chordTolerance==.0025&&fine->normalAngle==.075,"Fine settings");history=c.workspace().commands().size();check(c.dispatch_action("nurbs.source_display_fine")&&c.workspace().commands().size()==history,"No-op preset adds no history");
    const auto before=editor_nurbs_source(*o);const auto hash=o->polygon->contentHash;point=before->controls[5];point.y=1e7;check(!c.edit_nurbs_control(5,point)&&editor_nurbs_source(*o)==before&&o->polygon->contentHash==hash&&c.workspace().commands().size()==history,"Budget failure retains source/cache/history");
    check(c.dispatch_action("nurbs.source_quality")&&!editor_nurbs_source(*o)->adaptiveDisplay&&o->polygon->vertices.size()>4,"Switch to fixed density");check(c.dispatch_action("edit.undo")&&editor_nurbs_source(*o)==fine,"Undo fixed switch");
    const auto root=std::filesystem::temp_directory_path()/"dve_adaptive_display_test";std::filesystem::remove_all(root);std::filesystem::create_directories(root);auto save=c.workspace().document().save_transactional(root/"scene.dvescene");check(save.success,save.error);std::string error;auto loaded=EditorDocument::load(root/"scene.dvescene",&error);check(bool(loaded),error);check(editor_nurbs_source(*loaded->find_object(1))==fine&&loaded->find_object(1)->polygon->contentHash==o->polygon->contentHash&&loaded->validate(),"Reopen adaptive source/cache");std::filesystem::remove_all(root);
    o->flags.locked=true;history=c.workspace().commands().size();check(!c.dispatch_action("nurbs.source_display_standard")&&c.workspace().commands().size()==history,"Locked adaptive edit refuses");o->flags.locked=false;o->polygon->vertices[0].position.x+=.1F;check(!c.dispatch_action("nurbs.source_display_standard")&&c.workspace().commands().size()==history,"Stale cache refuses display change");
}
}
int main(){try{codec_geometry();native();std::cout<<"Adaptive display tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
