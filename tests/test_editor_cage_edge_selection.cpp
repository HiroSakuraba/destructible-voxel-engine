#include "dve/editor_subdivision.hpp"
#include "dve/editor_mesh_authoring.hpp"
#ifndef DVE_CAGE_SELECTION_CORE_ONLY
#include "dve/editor_native.hpp"
#include "dve/editor_runtime_settings.hpp"
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

void additional_topologies() {
    // Open cylinder rim: a closed boundary loop through regular valence-three vertices.
    auto f=grid(6,2,true); const auto& r=f.settings.controlFaces.front();
    const auto rim=edgeKey(vertex(f.mesh,f.mesh.vertices[r[0]].position),vertex(f.mesh,f.mesh.vertices[r[1]].position));
    const auto rimLoop=select_cage_edge_chain(f.mesh,f.settings,{rim},CageEdgeTraversal::Loop);
    check(bool(rimLoop)&&rimLoop.edges.size()==6&&rimLoop.stops.empty(),"closed boundary rim loop terminates once");
    auto g=grid(4,3);
    const auto vertical=key(g.mesh,{2,1,0},{2,2,0});
    const auto column=select_cage_edge_chain(g.mesh,g.settings,{vertical},CageEdgeTraversal::Loop);
    check(bool(column)&&column.edges.size()==3&&column.stops==std::set{CageEdgeStop::Boundary},"vertical loop spans grid");
    const auto row=select_cage_edge_chain(g.mesh,g.settings,{vertical},CageEdgeTraversal::Ring);
    check(bool(row)&&row.edges.size()==5,"vertical ring spans every column");
    const auto side=select_cage_edge_chain(g.mesh,g.settings,{key(g.mesh,{0,1,0},{0,2,0})},CageEdgeTraversal::Loop);
    check(bool(side)&&side.edges.size()==3,"side boundary loop stops at corners");
    for (std::uint32_t level : {1U,2U}) {
        auto settings=g.settings; settings.level=level;
        const auto leveled=select_cage_edge_chain(g.mesh,settings,{key(g.mesh,{1,1,0},{2,1,0})},CageEdgeTraversal::Loop);
        check(bool(leveled)&&leveled.edges.size()==4,"subdivision level does not change cage traversal");
    }
    const auto cube=make_editor_polygon_cube(1); SubdivisionSettings cubeSettings; freeze_subdivision_topology(cube,cubeSettings);
    for (auto e : subdivision_control_edges(cube,&cubeSettings)) {
        const auto ring=select_cage_edge_chain(cube,cubeSettings,{e},CageEdgeTraversal::Ring);
        check(bool(ring)&&ring.edges.size()==4,"every cube edge has a four-edge ring");
    }
}

void triangulate(Fixture& f) {
    f.settings.controlFaces.clear();
    for (std::size_t i=0;i<f.mesh.indices.size();i+=3)
        f.settings.controlFaces.push_back({f.mesh.indices[i],f.mesh.indices[i+1],f.mesh.indices[i+2]});
}
Fixture with_unused_vertices() {
    auto f=grid();f.mesh.vertices.insert(f.mesh.vertices.begin(),f.mesh.vertices.front());
    for (auto& index : f.mesh.indices) ++index;
    for (auto& face : f.settings.controlFaces) for (auto& index : face) ++index;
    auto orphan=f.mesh.vertices.front();orphan.position={100,100,100};f.mesh.vertices.push_back(orphan);
    f.mesh.bounds.maximum={100,100,100};return f;
}
void dissolve_and_cleanup_core() {
    auto f=grid();triangulate(f);
    const auto diagonal=key(f.mesh,{0,0,0},{1,1,0}),other=key(f.mesh,{2,0,0},{3,1,0});
    const auto before=polygon_asset_content_hash(f.mesh);
    const auto dissolved=dissolve_cage_edges(f.mesh,f.settings,{1,PolygonSelectionMode::Edge,{diagonal,other}});
    check(bool(dissolved),dissolved.error.c_str());
    check(dissolved.subdivision.controlFaces.size()==22&&dissolved.selection.components.size()==4,"batch dissolve produces two disjoint control quads");
    const auto edges=subdivision_control_edges(f.mesh,&dissolved.subdivision);
    check(!edges.contains(diagonal)&&!edges.contains(other),"dissolved diagonals disappear from authored cage selection");
    check(dissolved.lineage.edgeDescendants.at(diagonal).empty(),"dissolved edge is explicitly retired");
    check(dissolved.lineage.controlFaceDescendants.at(0)==dissolved.lineage.controlFaceDescendants.at(1),"triangle identities map to their common quad");
    check(polygon_asset_content_hash(f.mesh)==before,"dissolve retains every render vertex and attribute");
    const PolygonSelection seed{1,PolygonSelectionMode::Edge,{diagonal}};
    check(!dissolve_cage_edges(f.mesh,f.settings,seed,{diagonal}),"authored seam protects diagonal");
    auto creased=f.settings;creased.edgeCreases[diagonal]=2;
    check(!dissolve_cage_edges(f.mesh,creased,seed),"authored crease protects diagonal");
    check(!dissolve_cage_edges(f.mesh,f.settings,{1,PolygonSelectionMode::Edge,{key(f.mesh,{0,0,0},{1,0,0})}}),"boundary dissolve refuses removing the surface");
    check(!dissolve_cage_edges(f.mesh,f.settings,{1,PolygonSelectionMode::Edge,{diagonal,key(f.mesh,{1,0,0},{1,1,0})}}),"overlapping triangle pairs refuse as one batch");
    auto split=f;auto v=split.mesh.vertices[0];v.texcoord1.x=7;split.mesh.vertices.push_back(v);
    split.mesh.indices[3]=std::uint32_t(split.mesh.vertices.size()-1);split.settings.controlFaces[1][0]=split.mesh.indices[3];
    check(!dissolve_cage_edges(split.mesh,split.settings,seed),"UV1 discontinuity protects diagonal independently of UV0");
    auto material=f;material.mesh.materials.resize(2);material.mesh.materialBindings.resize(2);
    material.mesh.submeshes={{"A",0,3,0},{"B",3,3,1},{"Rest",6,std::uint32_t(material.mesh.indices.size()-6),0}};
    check(!dissolve_cage_edges(material.mesh,material.settings,seed),"material boundary protects diagonal");
    auto bent=f;for(auto& vertex:bent.mesh.vertices)if(vertex.position.x==0&&vertex.position.y==1)vertex.position.z=.3F;
    check(!dissolve_cage_edges(bent.mesh,bent.settings,seed),"nonplanar pair refused");
    const auto quads=grid();
    check(!dissolve_cage_edges(quads.mesh,quads.settings,{1,PolygonSelectionMode::Edge,{key(quads.mesh,{1,0,0},{1,1,0})}}),"quad-to-quad dissolve is deferred until larger face support");

    auto dirty=with_unused_vertices();const auto oldHash=polygon_asset_content_hash(dirty.mesh);
    const auto oldEdge=key(dirty.mesh,{1,1,0},{2,1,0});dirty.settings.edgeCreases[oldEdge]=2.5F;
    const auto compacted=compact_polygon_vertices(dirty.mesh,{1,PolygonSelectionMode::Vertex,{0}});
    check(bool(compacted),compacted.polygon.error.c_str());
    check(compacted.removedVertices==2&&compacted.polygon.mesh.vertices.size()==48,"only unreferenced vertices removed; used attribute splits remain");
    check(compacted.polygon.selection.components==std::set<std::uint64_t>{0},"orphan canonical alias maps to the surviving geometric vertex");
    check(compacted.polygon.mesh.indices==grid().mesh.indices,"triangle and corner ordering retained after compaction");
    for (std::size_t i=0;i<dirty.mesh.indices.size();++i) {
        const auto& a=dirty.mesh.vertices[dirty.mesh.indices[i]];const auto& b=compacted.polygon.mesh.vertices[compacted.polygon.mesh.indices[i]];
        check(a.position.x==b.position.x&&a.position.y==b.position.y&&a.position.z==b.position.z&&
            a.texcoord.x==b.texcoord.x&&a.texcoord.y==b.texcoord.y&&a.texcoord1.x==b.texcoord1.x&&a.texcoord1.y==b.texcoord1.y&&
            a.normal.x==b.normal.x&&a.normal.y==b.normal.y&&a.normal.z==b.normal.z&&a.tangent.x==b.tangent.x&&a.tangent.w==b.tangent.w,
            "referenced corner geometry, UVs, normals and tangents are exact");
    }
    PolygonUvAuthoring flags;flags.enabled=false;flags.layoutHash=polygon_uv_layout_hash(dirty.mesh);
    flags.seams[0]={oldEdge};flags.pins[0]={0};flags.pins[1]={7};
    std::string error;const auto mapped=remap_polygon_uv_authoring(flags,compacted.polygon.mesh,compacted.lineage,&error);
    check(bool(mapped),error.c_str());
    check(!mapped->enabled&&mapped->pins==flags.pins&&mapped->seams[0]==compacted.lineage.edgeDescendants.at(oldEdge),"compaction preserves disabled independent UV authoring flags");
    const auto settings=remap_compacted_cage(dirty.mesh,compacted.polygon.mesh,dirty.settings,compacted.lineage,&error);
    check(bool(settings),error.c_str());
    check(settings->controlFaces==grid().settings.controlFaces&&settings->edgeCreases.begin()->second==2.5F,"explicit control faces and authored crease remap without inference or decay");
    check(polygon_asset_content_hash(dirty.mesh)==oldHash,"cleanup computation leaves source unchanged");
    check(!compact_polygon_vertices(compacted.polygon.mesh,compacted.polygon.selection),"no-op cleanup is not reported as a mutation");
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

void native_cut_guards_and_input() {
    auto f=grid(); const auto seed=key(f.mesh,{1,1,0},{2,1,0});
    EditorDocument document("Loop cut guards"); EditorObject object(1,"Cage"); object.polygon=f.mesh; object.subdivision=f.settings;
    document.add_object(std::move(object));
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document))); auto& c=*owner;
    c.resize(1600,900); c.workspace().select_object(1); check(c.dispatch_action("subdiv.edge_mode"),"guards: edge mode");
    c.select_polygon_components({seed},SelectionOperation::Replace);
    const auto hash=[&]{ return polygon_asset_content_hash(*c.workspace().document().find_object(1)->polygon); };
    const auto source=hash();
    const auto view=c.layout().viewport; const int x=view.x+view.width/2,y=view.y+view.height/2;
    // Modal settings panel over the viewport receives the click; the slide is not captured.
    c.open_settings(SettingScope::User);
    check(c.dispatch_action("subdiv.loop_cut")&&!c.polygon_preview_lines().empty(),"guards: preview opens under settings panel");
    const auto centered=hash();
    check(centered!=source,"guards: preview geometry is live");
    c.pointer_down(PointerButton::Primary,x,y); c.pointer_move(x+view.width/4,y); c.pointer_up(PointerButton::Primary,x+view.width/4,y);
    check(hash()==centered,"settings panel click does not slide the loop cut");
    c.key_down("escape",false,false,false);
    if (c.polygon_preview_lines().empty()) {
        c.select_polygon_components({seed},SelectionOperation::Replace);
        check(c.dispatch_action("subdiv.loop_cut"),"guards: preview reopens after settings");
    }
    const auto reopened=hash();
    // Clicks outside the viewport (e.g. toolbar/outliner) do not start a slide either.
    c.pointer_down(PointerButton::Primary,2,2); c.pointer_move(x,y); c.pointer_up(PointerButton::Primary,x,y);
    check(hash()==reopened,"click outside viewport is not captured by the slide");
    check(!c.polygon_preview_lines().empty(),"guards: preview open for typed input");
    {
        // Typed values validate with the same 0.001..0.999 range as drag.
        for (auto k : {"5"}) c.key_down(k,false,false,false);
        c.update(0); c.refresh_menu_state();
        check(!std::as_const(c.workspace().menus()).find("subdiv.preview_apply")->enabled,"out-of-range typed position disables Apply");
        check(!c.dispatch_action("subdiv.preview_apply"),"out-of-range typed position refuses Apply");
        check(!c.polygon_preview_lines().empty(),"refused typed Apply keeps preview open");
        c.key_down("backspace",false,false,false);
        for (auto k : {"0",".","0","0","0","5"}) c.key_down(k,false,false,false);
        check(!c.dispatch_action("subdiv.preview_apply"),"typed position below 0.001 refused");
        c.key_down("escape",false,false,false);
    }
    check(hash()==source,"guards: escape restores source");
    // Apply refuses an isolated-out target without discarding the preview.
    c.select_polygon_components({seed},SelectionOperation::Replace);
    check(c.dispatch_action("subdiv.loop_cut"),"guards: isolated preview opens");
    const auto previewHash=hash(); const auto history=c.workspace().commands().size();
    c.viewport_settings().isolatedObjects={2};
    check(!c.dispatch_action("subdiv.preview_apply"),"isolated-out target refuses Apply");
    check(!c.polygon_preview_lines().empty()&&hash()==previewHash,"isolated refusal keeps the preview");
    check(c.workspace().commands().size()==history,"refused Apply adds no undo step");
    c.viewport_settings().isolatedObjects.clear();
    check(c.dispatch_action("subdiv.preview_apply"),"Apply succeeds once visible again");
    check(c.workspace().commands().size()==history+1,"loop cut is exactly one undo step");
    check(c.dispatch_action("edit.undo")&&hash()==source,"single undo restores the source cage");
    check(c.polygon_selection().components==std::set{seed},"single undo restores seed selection");
    // Locked and hidden cages refuse to open a preview.
    auto* target=c.workspace().document().find_object(1);
    target->flags.locked=true; check(!c.dispatch_action("subdiv.loop_cut")&&hash()==source,"locked cage refuses loop cut");
    target->flags.locked=false; target->flags.visible=false; check(!c.dispatch_action("subdiv.loop_cut"),"hidden cage refuses loop cut");
    target->flags.visible=true; c.viewport_settings().isolatedObjects={2};
    c.refresh_menu_state();
    check(!std::as_const(c.workspace().menus()).find("subdiv.loop_cut")->enabled,"isolated-out cage disables loop cut menu");
    check(!c.dispatch_action("subdiv.loop_cut"),"isolated-out cage refuses loop cut");
    c.viewport_settings().isolatedObjects.clear();
    c.refresh_menu_state();
    const auto results=c.workspace().menus().search("copy",8);
    check(!results.empty()&&results.front().id=="edit.copy","Edit > Copy stays first after loop cut commands");
}

void native_cleanup_history() {
    auto f=grid();triangulate(f);const auto diagonal=key(f.mesh,{0,0,0},{1,1,0});
    EditorDocument document("Dissolve history");EditorObject object(1,"Cage");object.polygon=f.mesh;object.subdivision=f.settings;
    document.add_object(std::move(object));
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));auto& c=*owner;
    c.resize(1600,900);c.workspace().select_object(1);check(c.dispatch_action("subdiv.edge_mode"),"dissolve edge mode");
    c.select_polygon_components({diagonal},SelectionOperation::Replace);
    const auto hash=polygon_asset_content_hash(f.mesh);const auto history=c.workspace().commands().size();
    check(c.dispatch_action("subdiv.dissolve_edges"),"dissolve opens preview");
    check(c.workspace().document().find_object(1)->subdivision->controlFaces.size()==23,"dissolve metadata is previewed");
    c.key_down("escape",false,false,false);
    check(*c.workspace().document().find_object(1)->subdivision==f.settings&&c.workspace().commands().size()==history,"dissolve Cancel restores settings without history");
    check(c.dispatch_action("subdiv.dissolve_edges")&&c.dispatch_action("subdiv.preview_apply"),"dissolve commits from menu");
    check(c.workspace().commands().size()==history+1&&c.polygon_selection().mode==PolygonSelectionMode::Face,"dissolve is one history step and selects merged face");
    check(polygon_asset_content_hash(*c.workspace().document().find_object(1)->polygon)==hash,"native dissolve keeps render attributes exact");
    check(c.dispatch_action("edit.undo")&&*c.workspace().document().find_object(1)->subdivision==f.settings&&c.polygon_selection().components==std::set{diagonal},"Undo restores triangle topology and seed edge");
    check(c.dispatch_action("edit.redo"),"dissolve Redo");check(c.dispatch_action("edit.undo"),"restore dissolve source");
    PolygonUvAuthoring flags;flags.layoutHash=polygon_uv_layout_hash(f.mesh);flags.seams[1]={diagonal};std::string error;
    auto command=make_polygon_uv_authoring_command(c.workspace().document(),*c.workspace().document().find_object(1),flags,&error);
    check(bool(command)&&c.workspace().commands().execute(c.workspace().document(),std::move(command)).success,"save protected UV1 seam");
    check(c.dispatch_action("subdiv.dissolve_edges")&&!c.dispatch_action("subdiv.preview_apply"),"UV1 seam blocks dissolve Apply");
    c.key_down("escape",false,false,false);

    auto dirty=with_unused_vertices();const auto oldEdge=key(dirty.mesh,{1,1,0},{2,1,0});dirty.settings.edgeCreases[oldEdge]=2;
    EditorDocument d("Cleanup history");EditorObject mesh(1,"Dirty cage");mesh.polygon=dirty.mesh;mesh.subdivision=dirty.settings;d.add_object(std::move(mesh));
    auto next=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(d)));auto& clean=*next;
    clean.resize(1600,900);clean.workspace().select_object(1);check(clean.dispatch_action("subdiv.edge_mode"),"cleanup edge mode");
    clean.select_polygon_components({oldEdge},SelectionOperation::Replace);
    flags={};flags.enabled=false;flags.layoutHash=polygon_uv_layout_hash(dirty.mesh);flags.seams[0]={oldEdge};flags.pins[1]={7};
    command=make_polygon_uv_authoring_command(clean.workspace().document(),*clean.workspace().document().find_object(1),flags,&error);
    check(bool(command)&&clean.workspace().commands().execute(clean.workspace().document(),std::move(command)).success,"save cleanup UV flags");
    const auto original=polygon_asset_content_hash(dirty.mesh);const auto count=clean.workspace().commands().size();
    const auto expected=compact_polygon_vertices(dirty.mesh,clean.polygon_selection());
    check(clean.dispatch_action("subdiv.clean_unused"),"cleanup native command connected");
    const auto* target=clean.workspace().document().find_object(1);const auto restoredFlags=read_polygon_uv_authoring(*target,&error);
    check(target->polygon->contentHash==expected.polygon.mesh.contentHash&&restoredFlags&&!restoredFlags->enabled&&restoredFlags->pins==flags.pins,"cleanup retains exact surviving attributes and saved UV pins");
    check(clean.workspace().commands().size()==count+1&&clean.polygon_selection().components==expected.polygon.selection.components,"cleanup geometry, metadata and selection are one transaction");
    check(clean.dispatch_action("edit.undo"),"cleanup Undo");target=clean.workspace().document().find_object(1);
    check(polygon_asset_content_hash(*target->polygon)==original&&*target->subdivision==dirty.settings&&clean.polygon_selection().components==std::set{oldEdge},"cleanup Undo restores exact geometry, topology, creases and edge identity");
    const auto undone=read_polygon_uv_authoring(*target,&error);check(undone&&undone->seams==flags.seams&&undone->pins==flags.pins,"cleanup Undo restores UV record");
    check(clean.dispatch_action("edit.redo"),"cleanup Redo");const auto noOpCount=clean.workspace().commands().size();
    check(!clean.dispatch_action("subdiv.clean_unused")&&clean.workspace().commands().size()==noOpCount,"repeated cleanup adds no false Undo entry");
    target=clean.workspace().document().find_object(1);
    clean.workspace().document().find_object(1)->flags.locked=true;check(!clean.dispatch_action("subdiv.clean_unused"),"locked mesh refuses cleanup");
    clean.workspace().document().find_object(1)->flags.locked=false;
}

void native_history_menus_and_search() {
    auto f=grid(); EditorDocument document("Cage selection history");
    EditorObject object(1,"Cage"); object.polygon=f.mesh; object.subdivision=f.settings;
    document.add_object(std::move(object));
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    auto& c=*owner;
    c.resize(1600,900); c.workspace().select_object(1);
    check(c.dispatch_action("subdiv.edge_mode"),"history: cage edge mode");
    const auto seed=key(f.mesh,{1,1,0},{2,1,0});
    c.select_polygon_components({seed},SelectionOperation::Replace);
    const auto history=c.workspace().commands().size();
    check(c.dispatch_action("subdiv.select_loop"),"history: loop command");
    check(c.dispatch_action("subdiv.select_loop"),"history: repeated loop command");
    check(c.dispatch_action("subdiv.select_ring"),"history: ring command");
    check(c.workspace().commands().size()==history && !c.workspace().commands().can_undo(),
        "selection traversal adds no geometry undo step");
    const auto disabled=[&](const char* id) {
        c.refresh_menu_state();
        const auto* action=std::as_const(c.workspace().menus()).find(id);
        return action && !action->enabled && !action->disabledReason.empty();
    };
    auto* cage=c.workspace().document().find_object(1);
    cage->flags.locked=true;
    check(disabled("subdiv.select_loop")&&disabled("polygon.select_ring"),"locked cage menus disabled with reason");
    cage->flags.locked=false; cage->flags.visible=false;
    check(disabled("subdiv.select_ring")&&disabled("polygon.select_loop"),"hidden cage menus disabled with reason");
    cage->flags.visible=true; c.viewport_settings().isolatedObjects={2};
    check(disabled("subdiv.select_loop")&&disabled("polygon.select_ring"),"isolated-out cage menus disabled with reason");
    c.viewport_settings().isolatedObjects.clear();
    c.refresh_menu_state();
    // The new cage commands mention polygons/convert; they must not outrank Edit > Copy.
    const auto results=c.workspace().menus().search("copy",8);
    check(!results.empty()&&results.front().id=="edit.copy","Edit > Copy stays first for copy search");
}
#endif
}
int main() {
    try {
        open_strip_and_seams(); closed_cycles_and_reordering(); stops_and_atomic_refusals(); additional_topologies(); cuts_slide_and_lineage(); dissolve_and_cleanup_core();
#ifndef DVE_CAGE_SELECTION_CORE_ONLY
        native_commands_and_guards(); native_history_menus_and_search(); native_cut_history_and_preview(); native_cut_guards_and_input(); native_cleanup_history();
#endif
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    std::cout<<"Cage selection, cut/slide, dissolve and cleanup regression checks passed\n";
}
