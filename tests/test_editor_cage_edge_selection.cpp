#include "dve/editor_subdivision.hpp"
#include "dve/editor_mesh_authoring.hpp"
#ifndef DVE_CAGE_SELECTION_CORE_ONLY
#include "dve/editor_native.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <tuple>

using namespace dve;
using namespace dve::editor;
namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    CookedPolygonAsset mesh;
    SubdivisionSettings settings;
};
Fixture grid(unsigned width=4,unsigned height=3,bool cylinder=false) {
    Fixture f; f.mesh.objectId=1; f.mesh.materials.resize(1); f.mesh.materialBindings.resize(1);
    f.settings.level=0;
    const auto point=[&](unsigned x,unsigned y) {
        if (!cylinder) return Float3{float(x),float(y),0};
        const float theta=float(x%width)*6.28318530718F/float(width);
        return Float3{std::cos(theta),float(y),std::sin(theta)};
    };
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const auto base=std::uint32_t(f.mesh.vertices.size());
        const Float3 points[]={point(x,y),point(x+1,y),point(x+1,y+1),point(x,y+1)};
        for (unsigned j=0;j<4;++j) {
            PolygonVertex v; v.position=points[j]; v.normal={0,0,1};
            // Every quad has separate UV/normal attribute vertices at geometric seams.
            v.texcoord={float(j%2)+float(x),float(j/2)+float(y)};
            f.mesh.vertices.push_back(v);
        }
        const std::vector<std::uint32_t> record={base,base+1,base+2,base,base+2,base+3};
        f.mesh.indices.insert(f.mesh.indices.end(),record.begin(),record.end());
        f.settings.controlFaces.push_back(record);
    }
    f.mesh.submeshes={{"Grid",0,std::uint32_t(f.mesh.indices.size()),0}};
    f.mesh.bounds={{-2,-2,-2},{float(width+2),float(height+2),2}};
    return f;
}
std::uint32_t vertex(const CookedPolygonAsset& mesh,Float3 p) {
    for (std::uint32_t i=0;i<mesh.vertices.size();++i)
        if (mesh.vertices[i].position.x==p.x && mesh.vertices[i].position.y==p.y && mesh.vertices[i].position.z==p.z) return i;
    throw std::runtime_error("Fixture point missing");
}
std::uint64_t edgeKey(std::uint32_t a,std::uint32_t b) {
    const auto [lo,hi]=std::minmax(a,b); return (std::uint64_t(lo)<<32U)|hi;
}
std::uint64_t key(const CookedPolygonAsset& mesh,Float3 a,Float3 b) { return edgeKey(vertex(mesh,a),vertex(mesh,b)); }

void open_strip_and_seams() {
    const auto f=grid(); const auto before=polygon_asset_content_hash(f.mesh);
    const auto seed=key(f.mesh,{1,1,0},{2,1,0});
    const auto loop=select_cage_edge_chain(f.mesh,f.settings,{seed},CageEdgeTraversal::Loop);
    std::set<std::uint64_t> expectedLoop,expectedRing;
    for (unsigned x=0;x<4;++x) expectedLoop.insert(key(f.mesh,{float(x),1,0},{float(x+1),1,0}));
    for (unsigned y=0;y<=3;++y) expectedRing.insert(key(f.mesh,{1,float(y),0},{2,float(y),0}));
    check(bool(loop)&&loop.edges==expectedLoop,"loop follows quad vertices across split attributes");
    check(loop.stops==std::set{CageEdgeStop::Boundary},"open loop reports boundary");
    const auto ring=select_cage_edge_chain(f.mesh,f.settings,{seed},CageEdgeTraversal::Ring);
    check(bool(ring)&&ring.edges==expectedRing,"ring crosses opposite quad edges");
    check(ring.stops==std::set{CageEdgeStop::Boundary},"open ring reports boundary");
    const auto boundary=select_cage_edge_chain(f.mesh,f.settings,{key(f.mesh,{1,0,0},{2,0,0})},CageEdgeTraversal::Loop);
    check(bool(boundary)&&boundary.edges.size()==4,"regular boundary continues until corner");
    const auto second=key(f.mesh,{1,2,0},{2,2,0});
    const auto multi=select_cage_edge_chain(f.mesh,f.settings,{seed,second},CageEdgeTraversal::Loop);
    check(bool(multi)&&multi.edges.size()==8,"multiple seeds union distinct chains");
    const auto repeated=select_cage_edge_chain(f.mesh,f.settings,multi.edges,CageEdgeTraversal::Loop);
    check(bool(repeated)&&repeated.edges==multi.edges,"selection expansion is idempotent");
    const auto allowed=subdivision_control_edges(f.mesh,&f.settings);
    check(std::includes(allowed.begin(),allowed.end(),ring.edges.begin(),ring.edges.end()),"render diagonals excluded");
    check(polygon_asset_content_hash(f.mesh)==before,"selection does not mutate mesh attributes");
}

void closed_cycles_and_reordering() {
    auto f=grid(6,3,true);
    const auto& record=f.settings.controlFaces[6];
    const auto seed=edgeKey(vertex(f.mesh,f.mesh.vertices[record[0]].position),vertex(f.mesh,f.mesh.vertices[record[1]].position));
    const auto loop=select_cage_edge_chain(f.mesh,f.settings,{seed},CageEdgeTraversal::Loop);
    check(bool(loop)&&loop.edges.size()==6&&loop.stops.empty(),"closed cylinder loop terminates once");
    // Vertical edges form a closed ring around a quad cylinder band.
    const auto vertical=edgeKey(vertex(f.mesh,f.mesh.vertices[record[1]].position),vertex(f.mesh,f.mesh.vertices[record[2]].position));
    const auto ring=select_cage_edge_chain(f.mesh,f.settings,{vertical},CageEdgeTraversal::Ring);
    check(bool(ring)&&ring.edges.size()==6&&ring.stops.empty(),"closed cylinder ring terminates once");
    std::vector<std::uint32_t> reordered;
    for (std::size_t i=f.mesh.indices.size();i>0;i-=3)
        reordered.insert(reordered.end(),f.mesh.indices.begin()+std::ptrdiff_t(i-3),f.mesh.indices.begin()+std::ptrdiff_t(i));
    f.mesh.indices=std::move(reordered);
    std::reverse(f.settings.controlFaces.begin(),f.settings.controlFaces.end());
    const auto after=select_cage_edge_chain(f.mesh,f.settings,{seed},CageEdgeTraversal::Loop);
    check(bool(after)&&after.edges==loop.edges&&after.stops==loop.stops,"saved faces resolve independently of render ordering");
}

void stops_and_atomic_refusals() {
    auto f=grid(); const auto seed=key(f.mesh,{1,1,0},{2,1,0});
    const auto limited=select_cage_edge_chain(f.mesh,f.settings,{seed},CageEdgeTraversal::Ring,2);
    check(!limited&&limited.edges.empty()&&limited.stops.empty(),"budget refusal has no partial result");
    check(!select_cage_edge_chain(f.mesh,f.settings,{seed},CageEdgeTraversal::Loop,0),"zero budget refused");
    const auto diagonal=edgeKey(0,2);
    check(!select_cage_edge_chain(f.mesh,f.settings,{diagonal},CageEdgeTraversal::Ring),"hidden diagonal cannot seed traversal");
    const auto stale=select_cage_edge_chain(f.mesh,f.settings,{seed,0xffffffffffffffffULL},CageEdgeTraversal::Loop);
    check(!stale&&stale.edges.empty(),"mixed valid/stale seeds refuse atomically");
    check(!select_cage_edge_chain(f.mesh,f.settings,{},CageEdgeTraversal::Loop),"empty seed set refused");
    auto missing=f.settings; missing.controlFaces.clear();
    check(!select_cage_edge_chain(f.mesh,missing,{seed},CageEdgeTraversal::Ring),"missing saved faces are not guessed");
    auto malformed=f.settings; malformed.controlFaces[0].pop_back();
    check(!select_cage_edge_chain(f.mesh,malformed,{seed},CageEdgeTraversal::Ring),"malformed metadata refused");
    auto incomplete=f.settings; incomplete.controlFaces.pop_back();
    check(!select_cage_edge_chain(f.mesh,incomplete,{seed},CageEdgeTraversal::Ring),"incomplete metadata refused");
    // Explicitly triangulate a neighboring quad: the chain must stop rather than
    // guessing across its render diagonal or branching into the rest of the shell.
    auto triangles=f.settings; const auto first=triangles.controlFaces[0];
    triangles.controlFaces[0]={first[0],first[1],first[2]};
    triangles.controlFaces.push_back({first[3],first[4],first[5]});
    const auto stopped=select_cage_edge_chain(f.mesh,triangles,{key(f.mesh,{0,1,0},{1,1,0})},CageEdgeTraversal::Ring);
    check(bool(stopped)&&stopped.stops.contains(CageEdgeStop::Triangle),"ring reports triangle stop");
    const auto stoppedLoop=select_cage_edge_chain(f.mesh,triangles,{seed},CageEdgeTraversal::Loop);
    check(bool(stoppedLoop)&&stoppedLoop.stops.contains(CageEdgeStop::Triangle),"loop reports triangle stop");
    // Duplicate a face sharing existing edges: refuses nonmanifold topology globally.
    auto nonmanifold=f;
    nonmanifold.mesh.indices.insert(nonmanifold.mesh.indices.end(),first.begin(),first.end());
    nonmanifold.settings.controlFaces.push_back(first);
    nonmanifold.mesh.submeshes[0].indexCount=std::uint32_t(nonmanifold.mesh.indices.size());
    const auto invalid=select_cage_edge_chain(nonmanifold.mesh,nonmanifold.settings,{seed},CageEdgeTraversal::Ring);
    check(!invalid&&invalid.edges.empty(),"nonmanifold cage refused without partial selection");
    const auto cube=make_editor_polygon_cube(1); SubdivisionSettings cubeSettings; freeze_subdivision_topology(cube,cubeSettings);
    const auto poleSeed=*subdivision_control_edges(cube,&cubeSettings).begin();
    const auto pole=select_cage_edge_chain(cube,cubeSettings,{poleSeed},CageEdgeTraversal::Loop);
    check(bool(pole)&&pole.edges==std::set{poleSeed}&&pole.stops.contains(CageEdgeStop::Pole),"cube pole is not guessed through");
    const auto cubeRing=select_cage_edge_chain(cube,cubeSettings,{poleSeed},CageEdgeTraversal::Ring);
    check(bool(cubeRing)&&cubeRing.edges.size()==4&&cubeRing.stops.empty(),"closed cube ring follows saved quads");
}

void cuts_slide_and_lineage() {
    auto f=grid(); const auto seed=key(f.mesh,{1,1,0},{2,1,0});
    f.settings.edgeCreases[seed]=2.5F;
    const auto before=polygon_asset_content_hash(f.mesh);
    const auto cut=cut_cage_edge_ring(f.mesh,f.settings,seed,.25F);
    check(bool(cut),cut.polygon.error.c_str());
    check(cut.subdivision.controlFaces.size()==15 && cut.polygon.mesh.indices.size()==90,"strip emits explicit child quads");
    check(cut.polygon.selection.components.size()==3,"new cut loop is selected");
    for (auto edge : cut.polygon.selection.components) {
        check(std::abs(cut.polygon.mesh.vertices[std::size_t(edge>>32)].position.x-1.25F)<1e-5F,"oriented slide places first endpoint");
        check(std::abs(cut.polygon.mesh.vertices[std::uint32_t(edge)].position.x-1.25F)<1e-5F,"oriented slide places second endpoint");
        check(!cut.subdivision.edgeCreases.contains(edge),"new transverse edges default smooth");
    }
    check(cut.lineage.vertexSurvivors.size()==f.mesh.vertices.size(),"old attribute vertices retain indices");
    const auto& split=cut.lineage.edgeDescendants.at(seed);
    check(split.size()==2&&cut.subdivision.edgeCreases.size()==2,"creased edge splits into two children");
    for (auto child : split) check(cut.subdivision.edgeCreases.at(child)==2.5F,"split children inherit authored strength without evaluation decay");
    check(cut.lineage.controlFaceDescendants.at(1).size()==2,"touched control face records its children");
    check(cut.lineage.controlFaceDescendants.at(0).size()==1,"untouched control face retains a single descendant");
    PolygonUvAuthoring flags; flags.layoutHash=polygon_uv_layout_hash(f.mesh); flags.enabled=false;
    flags.seams[0]={seed}; flags.pins[0]={6}; flags.pins[1]={0};
    flags.seams[1]={key(f.mesh,{0,3,0},{1,3,0})};
    std::string error;
    const auto mapped=remap_polygon_uv_authoring(flags,cut.polygon.mesh,cut.lineage,&error);
    check(bool(mapped),error.c_str());
    check(!mapped->enabled&&mapped->seams[0]==split,"disabled saved seam flags follow both split children");
    for (unsigned channel=0;channel<2;++channel) for (auto old : flags.pins[channel]) {
        const auto p=f.mesh.vertices[f.mesh.indices[old]];
        for (auto descendant : cut.lineage.cornerDescendants.at(old)) {
            check(mapped->pins[channel].contains(descendant),"pinned corner follows explicit lineage");
            const auto q=cut.polygon.mesh.vertices[cut.polygon.mesh.indices[descendant]];
            check(p.texcoord.x==q.texcoord.x&&p.texcoord.y==q.texcoord.y&&p.texcoord1.x==q.texcoord1.x&&p.texcoord1.y==q.texcoord1.y,"surviving pinned UV coordinates retained");
        }
    }
    auto internal=flags; internal.seams[0]={edgeKey(4,6)};
    check(!remap_polygon_uv_authoring(internal,cut.polygon.mesh,cut.lineage,&error),"retired internal seam refuses metadata remapping");
    check(polygon_asset_content_hash(f.mesh)==before,"cut computation does not mutate source");
    const auto slide=cut_cage_edge_ring(f.mesh,f.settings,seed,.75F);
    check(bool(slide)&&slide.polygon.mesh.indices==cut.polygon.mesh.indices,"slide preview updates positions without changing generated connectivity");
    check(slide.subdivision.controlFaces==cut.subdivision.controlFaces,"slide preserves authored child-face identities");
    const auto cube=make_editor_polygon_cube(1); SubdivisionSettings cubeSettings; cubeSettings.level=0; freeze_subdivision_topology(cube,cubeSettings);
    const auto cubeCut=cut_cage_edge_ring(cube,cubeSettings,*subdivision_control_edges(cube,&cubeSettings).begin());
    check(bool(cubeCut)&&cubeCut.subdivision.controlFaces.size()==10&&cubeCut.polygon.selection.components.size()==4,"closed strip cuts into a closed selected loop");
    check(bool(select_cage_edge_chain(cubeCut.polygon.mesh,cubeCut.subdivision,cubeCut.polygon.selection.components,CageEdgeTraversal::Loop)),"generated topology supports loop selection");
    for (float value : {0.F,1.F,-.2F,std::numeric_limits<float>::infinity()})
        check(!cut_cage_edge_ring(f.mesh,f.settings,seed,value),"invalid slide fraction refused");
    auto triangle=f.settings; const auto record=triangle.controlFaces[1];
    triangle.controlFaces[1]={record[0],record[1],record[2]}; triangle.controlFaces.push_back({record[3],record[4],record[5]});
    check(!cut_cage_edge_ring(f.mesh,triangle,seed),"triangle incident to cut ring refuses whole cut");
    auto bent=f;
    for (auto& v : bent.mesh.vertices) if (v.position.x==1&&v.position.y==1) v.position.z=.3F;
    check(!cut_cage_edge_ring(bent.mesh,bent.settings,seed),"nonplanar strip refused in initial supported scope");
}

#ifndef DVE_CAGE_SELECTION_CORE_ONLY
void native_commands_and_guards() {
    auto f=grid(); EditorDocument document("Cage selection regression");
    EditorObject object(1,"Cage"); object.polygon=f.mesh; object.subdivision=f.settings;
    document.add_object(std::move(object));
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    auto& c=*owner;
    c.resize(1600,900); c.workspace().select_object(1);
    check(c.dispatch_action("subdiv.edge_mode"),"native cage edge mode");
    const auto seed=key(f.mesh,{1,1,0},{2,1,0});
    c.select_polygon_components({seed},SelectionOperation::Replace);
    c.update(0);
    const auto* menu=std::as_const(c.workspace().menus()).find("subdiv.select_loop");
    check(menu&&menu->enabled,"loop menu enabled for selected cage edges");
    check(c.dispatch_action("subdiv.select_loop")&&c.polygon_selection().components.size()==4,"native loop command connected");
    c.select_polygon_components({seed},SelectionOperation::Replace);
    check(c.dispatch_action("subdiv.select_ring")&&c.polygon_selection().components.size()==4,"native ring command connected");
    const auto expected=c.polygon_selection().components;
    c.workspace().document().find_object(1)->flags.locked=true;
    check(!c.dispatch_action("subdiv.select_loop")&&c.polygon_selection().components==expected,"locked command does not replace selection");
    c.workspace().document().find_object(1)->flags.locked=false;
    c.workspace().document().find_object(1)->flags.visible=false;
    check(!c.dispatch_action("subdiv.select_ring"),"hidden cage refuses traversal");
    c.workspace().document().find_object(1)->flags.visible=true;
    c.viewport_settings().isolatedObjects={2};
    check(!c.dispatch_action("subdiv.select_loop"),"isolated-out cage refuses traversal");
    c.viewport_settings().isolatedObjects.clear();
    check(c.dispatch_action("subdiv.face_mode"),"switch away from edges");
    check(!c.dispatch_action("subdiv.select_ring"),"face mode refuses edge ring");
    check(c.dispatch_action("subdiv.edge_mode"),"restore edge mode");
    c.select_polygon_components({seed},SelectionOperation::Replace);
    c.workspace().document().find_object(1)->subdivision.reset();
    check(!c.dispatch_action("polygon.select_loop"),"ordinary triangles are not guessed into quads");
}
void native_cut_history_and_preview() {
    auto f=grid(); const auto seed=key(f.mesh,{1,1,0},{2,1,0}); f.settings.edgeCreases[seed]=2;
    EditorDocument document("Loop cut history"); EditorObject object(1,"Cage"); object.polygon=f.mesh; object.subdivision=f.settings;
    document.add_object(std::move(object));
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document))); auto& c=*owner;
    c.resize(1600,900); c.workspace().select_object(1); check(c.dispatch_action("subdiv.edge_mode"),"enter cut edge mode");
    c.select_polygon_components({seed},SelectionOperation::Replace);
    const auto before=polygon_asset_content_hash(*c.workspace().document().find_object(1)->polygon);
    PolygonUvAuthoring flags; flags.layoutHash=polygon_uv_layout_hash(f.mesh); flags.seams[0]={seed}; flags.pins[0]={6};
    std::string error;
    auto uvCommand=make_polygon_uv_authoring_command(c.workspace().document(),*c.workspace().document().find_object(1),flags,&error);
    check(bool(uvCommand),error.c_str()); check(c.workspace().commands().execute(c.workspace().document(),std::move(uvCommand)).success,"save source UV authoring flags");
    check(c.dispatch_action("subdiv.loop_cut"),"native cut opens preview");
    check(!c.dispatch_action("file.save"),"save blocked while cut is unapplied");
    const auto view=c.layout().viewport; const int x=view.x+view.width/2,y=view.y+view.height/2;
    c.pointer_down(PointerButton::Primary,x,y); c.pointer_move(x+view.width/8,y); c.pointer_up(PointerButton::Primary,x+view.width/8,y);
    const auto after=polygon_asset_content_hash(*c.workspace().document().find_object(1)->polygon);
    check(after!=before,"drag slides new cut geometry");
    check(c.dispatch_action("subdiv.preview_apply"),"native cut applies once");
    const auto* target=c.workspace().document().find_object(1);
    const auto afterSettings=*target->subdivision;
    const auto afterFlags=read_polygon_uv_authoring(*target,&error); check(bool(afterFlags),error.c_str());
    check(afterFlags->seams[0].size()==2&&!afterFlags->pins[0].empty(),"native commit writes remapped seam and pin state");
    const auto root=std::filesystem::temp_directory_path()/"dve-cage-loop-cut-history";
    std::filesystem::create_directories(root);
    const auto saved=c.workspace().document().save_transactional(root/"cut.dvescene");
    check(saved.success,saved.error.c_str());
    auto loaded=EditorDocument::load(root/"cut.dvescene",&error); check(bool(loaded),error.c_str());
    const auto* reopened=loaded->find_object(1);
    check(reopened&&reopened->subdivision==target->subdivision,"save/reopen retains explicit cut faces and inherited creases");
    const auto reopenedFlags=read_polygon_uv_authoring(*reopened,&error);
    check(bool(reopenedFlags)&&reopenedFlags->seams==afterFlags->seams&&reopenedFlags->pins==afterFlags->pins,"save/reopen retains remapped seams and pins");
    std::filesystem::remove_all(root);
    check(c.dispatch_action("edit.undo"),"cut undo"); target=c.workspace().document().find_object(1);
    check(polygon_asset_content_hash(*target->polygon)==before&&*target->subdivision==f.settings,"undo restores geometry and authored settings");
    check(c.polygon_selection().components==std::set{seed},"undo restores seed selection");
    const auto oldFlags=read_polygon_uv_authoring(*target,&error); check(bool(oldFlags)&&oldFlags->seams==flags.seams&&oldFlags->pins==flags.pins,"undo restores saved UV state");
    check(c.dispatch_action("edit.redo"),"cut redo"); target=c.workspace().document().find_object(1);
    check(polygon_asset_content_hash(*target->polygon)==after&&*target->subdivision==afterSettings,"redo restores slid geometry and explicit child faces");
    check(c.dispatch_action("edit.undo"),"restore before preview cancellation");
    check(c.dispatch_action("subdiv.loop_cut"),"second preview opens"); c.key_down("escape",false,false,false);
    check(polygon_asset_content_hash(*c.workspace().document().find_object(1)->polygon)==before,"Cancel restores original cage");
    check(c.dispatch_action("subdiv.loop_cut"),"focus-loss preview opens"); c.clear_navigation_input();
    check(polygon_asset_content_hash(*c.workspace().document().find_object(1)->polygon)==before,"focus loss cancels loop cut");
    check(c.dispatch_action("subdiv.face_mode")&&c.dispatch_action("subdiv.inset"),"open a previous Inset tool");
    c.key_down("escape",false,false,false);
    check(c.dispatch_action("subdiv.edge_mode"),"return to cut seed selection"); c.select_polygon_components({seed},SelectionOperation::Replace);
    check(c.dispatch_action("subdiv.loop_cut"),"cut after Inset opens"); c.key_down("up",false,false,false);
    const auto firstEdge=*c.polygon_selection().components.begin();
    check(std::abs(c.workspace().document().find_object(1)->polygon->vertices[std::size_t(firstEdge>>32)].position.x-1.51F)<1e-5F,"arrows adjust loop position after a previous Inset");
    c.key_down("escape",false,false,false);
    auto* current=c.workspace().document().find_object(1);
    const auto savedRecord=current->components[0].properties.at("state");
    check(c.dispatch_action("subdiv.loop_cut"),"stale UV preview opens");
    current->components[0].properties["state"]=std::string("invalid inspector edit");
    check(!c.dispatch_action("subdiv.preview_apply"),"Apply revalidates inspector changes to UV metadata");
    check(polygon_asset_content_hash(*current->polygon)==before,"stale UV rejection restores source geometry");
    c.key_down("escape",false,false,false); current->components[0].properties["state"]=savedRecord;
}
#endif
}
int main() {
    try {
        open_strip_and_seams(); closed_cycles_and_reordering(); stops_and_atomic_refusals(); cuts_slide_and_lineage();
#ifndef DVE_CAGE_SELECTION_CORE_ONLY
        native_commands_and_guards(); native_cut_history_and_preview();
#endif
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    std::cout<<"Cage edge selection, cut/slide and lineage regression checks passed\n";
}
