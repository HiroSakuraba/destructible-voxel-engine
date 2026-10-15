#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_modeling_topology.hpp"
#include "dve/editor_subdivision.hpp"
#include "dve/editor_command.hpp"
#include <memory>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <filesystem>
#ifndef DVE_EDGE_BEVEL_CORE_ONLY
#include "dve/editor_native.hpp"
#endif
using namespace dve;using namespace dve::editor;
namespace {
void check(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
std::uint64_t key(std::uint32_t a,std::uint32_t b){return (std::uint64_t((std::min)(a,b))<<32U)|(std::max)(a,b);}
struct Fixture{CookedPolygonAsset mesh;SubdivisionSettings settings;PolygonSelection selection;};
Fixture cube(){Fixture f;f.mesh=make_editor_polygon_cube(1);f.settings.level=0;freeze_subdivision_topology(f.mesh,f.settings);auto e=*subdivision_control_edges(f.mesh,&f.settings).begin();f.selection={1,PolygonSelectionMode::Edge,{e}};return f;}
Fixture fold(bool concave,unsigned pieces=1,bool boundary=false){Fixture f;f.mesh.objectId=1;f.mesh.materials.resize(1);f.mesh.materialBindings.resize(1);f.settings.level=0;
    const auto add=[&](Float3 p){PolygonVertex v;v.position=p;v.texcoord={p.x,p.y+p.z};v.texcoord1={p.x*2+.3F,(p.y+p.z)*3+.7F};v.color={.2F,.4F,.6F,.8F};f.mesh.vertices.push_back(v);return std::uint32_t(f.mesh.vertices.size()-1);};
    for(unsigned j=0;j<=pieces;++j){add({float(j),0,0});add({float(j),1,0});if(!boundary)add({float(j),0,concave?1.F:-1.F});}
    const auto stride=boundary?2U:3U;f.selection={1,PolygonSelectionMode::Edge,{}};
    for(unsigned j=0;j<pieces;++j){auto a=j*stride,b=(j+1)*stride;std::vector<std::uint32_t> first{a,b,b+1,a,b+1,a+1};f.mesh.indices.insert(f.mesh.indices.end(),first.begin(),first.end());f.settings.controlFaces.push_back(first);
        if(!boundary){std::vector<std::uint32_t> second{b,a,a+2,b,a+2,b+2};f.mesh.indices.insert(f.mesh.indices.end(),second.begin(),second.end());f.settings.controlFaces.push_back(second);}f.selection.components.insert(key(a,b));}
    f.mesh.submeshes={{"Fold",0,std::uint32_t(f.mesh.indices.size()),0}};refresh_editor_polygon(f.mesh,true);return f;}
Fixture segmented_cube(unsigned cuts=1){
    auto f=cube();for(auto& v:f.mesh.vertices)v.texcoord1={2*v.texcoord.x+.3F,3*v.texcoord.y+.7F};refresh_editor_polygon(f.mesh);
    for(unsigned i=0;i<cuts;++i){
        const auto length=[&](std::uint64_t e){return length_squared(subtract(f.mesh.vertices[std::uint32_t(e>>32U)].position,f.mesh.vertices[std::uint32_t(e)].position));};
        const auto seed=*std::max_element(f.selection.components.begin(),f.selection.components.end(),[&](auto a,auto b){return length(a)<length(b);});
        auto cut=cut_cage_edge_ring(f.mesh,f.settings,seed,.5F);check(bool(cut),"segmented cube loop cut: "+cut.polygon.error);
        std::set<std::uint64_t> next;for(auto e:f.selection.components){const auto& children=cut.lineage.edgeDescendants.at(e);next.insert(children.begin(),children.end());}
        f.mesh=std::move(cut.polygon.mesh);f.settings=std::move(cut.subdivision);f.selection.components=std::move(next);
    }
    return f;
}
void no_t_junctions(const CookedPolygonAsset& mesh,const std::string& context={}){
    const auto groups=polygon_geometric_vertices(mesh);std::map<std::uint32_t,Float3> points;
    for(std::uint32_t i=0;i<groups.size();++i)points[groups[i]]=mesh.vertices[i].position;
    for(auto e:polygon_all_components(mesh,PolygonSelectionMode::Edge)){
        const auto a=std::uint32_t(e>>32U),b=std::uint32_t(e);const auto p=points.at(a),d=subtract(points.at(b),p);const auto dd=dot(d,d);
        for(const auto& [g,point]:points)if(g!=a&&g!=b){const auto u=dot(subtract(point,p),d)/dd;
            check(!(u>1e-5F&&u<1-1e-5F&&length_squared(subtract(point,add(p,multiply(d,u))))<1e-12F),"closed bevel has no T-junctions: "+context+" edge="+std::to_string(e)+" point="+std::to_string(g)+" u="+std::to_string(u)+" A="+std::to_string(p.x)+","+std::to_string(p.y)+","+std::to_string(p.z)+" B="+std::to_string(points.at(b).x)+","+std::to_string(points.at(b).y)+","+std::to_string(points.at(b).z)+" P="+std::to_string(point.x)+","+std::to_string(point.y)+","+std::to_string(point.z));}
    }
}
double volume(const CookedPolygonAsset& m){double value=0;for(std::size_t i=0;i<m.indices.size();i+=3){auto a=m.vertices[m.indices[i]].position,b=m.vertices[m.indices[i+1]].position,c=m.vertices[m.indices[i+2]].position;value+=double(a.x)*(double(b.y)*c.z-double(b.z)*c.y)+double(a.y)*(double(b.z)*c.x-double(b.x)*c.z)+double(a.z)*(double(b.x)*c.y-double(b.y)*c.x);}return value/6;}
void run(Fixture f,bool closed){for(auto& vertex:f.mesh.vertices)vertex.texcoord1={2*vertex.texcoord.x+.3F,3*vertex.texcoord.y+.7F};refresh_editor_polygon(f.mesh);std::string error;const auto seed=*f.selection.components.begin();f.settings.edgeCreases[seed]=2;const auto original=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(original),error);
    for(unsigned segments:{1U,4U}){EdgeBevelSettings s;s.width=.1F;s.offsetType=BevelOffsetType::Absolute;s.worldSpace=false;s.autoFit=false;s.depth=segments==1?0.F:1.F;s.segments=segments;
        auto result=bevel_authored_editor_edges(f.mesh,f.selection,s,{1,1,1},&f.settings);check(bool(result),std::string(closed?"closed":"open")+" segments="+std::to_string(segments)+": "+result.polygon.error);
        for(const auto& vertex:result.polygon.mesh.vertices)check(std::abs(vertex.texcoord1.x-(2*vertex.texcoord.x+.3F))<1e-5F&&std::abs(vertex.texcoord1.y-(3*vertex.texcoord.y+.7F))<1e-5F,"bevel preserves independent UV1 interpolation rather than overwriting it with UV0");
        const auto diagnostic=diagnose_polygon_topology(result.polygon.mesh);check(!diagnostic.nonmanifold&&!diagnostic.inconsistentWinding&&!diagnostic.degenerate&&(!closed||!diagnostic.boundaries),"bevel has valid topology");
        auto ids=advance_modeling_topology(f.mesh,result.polygon.mesh,&*result.subdivision,*original,result.lineage);check(bool(ids),ids.error);check(!ids.created.empty(),"bevel allocates new explicit IDs");
        check(!result.lineage.geometricVertexCopies.empty(),"removed bevel corners declare explicit replacement geometry descendants");
        check(!result.lineage.edgeDescendants.at(seed).empty(),"selected edge has replacement rail descendants");for(auto edge:result.lineage.edgeDescendants.at(seed))check(result.subdivision->edgeCreases.at(edge)==2,"replacement rails inherit crease strength");
        PolygonUvAuthoring flags;flags.layoutHash=polygon_uv_layout_hash(f.mesh);flags.enabled=false;flags.seams[0]={seed};flags.seams[1]={seed};
        auto mapped=remap_polygon_uv_authoring(flags,result.polygon.mesh,result.lineage,&error);check(mapped&&mapped->seams[0]==result.lineage.edgeDescendants.at(seed)&&mapped->seams[1]==mapped->seams[0]&&!mapped->enabled,"both seam channels follow explicit rails");
        for(const auto& [old,next]:result.lineage.vertexSurvivors)check(ids.topology.vertices[next]==original->vertices[old]&&ids.topology.attributes[next]==original->attributes[old],"untouched geometric and attribute IDs survive");
        auto again=s;again.width=.15F;auto changed=bevel_authored_editor_edges(f.mesh,f.selection,again,{1,1,1},&f.settings);check(bool(changed),changed.polygon.error);auto repeated=advance_modeling_topology(f.mesh,changed.polygon.mesh,&*changed.subdivision,*original,changed.lineage);check(bool(repeated),repeated.error);check(ids.topology==repeated.topology,"width adjustment retains provisional IDs on supported connectivity");
        if(closed&&segments==1)check(std::abs(volume(result.polygon.mesh)-.99)<1e-5,"closed-convex chamfer preserves legacy width/volume semantics");
        auto evaluated=*result.subdivision;evaluated.level=1;const auto evaluation=evaluate_subdivision(result.polygon.mesh,evaluated);check(bool(evaluation.mesh),"beveled control faces subdivide: "+evaluation.error);
    }
}

void split_crease_chains(){
    for(unsigned cuts:{1U,2U,3U})for(unsigned segments:{1U,4U,16U}){
        auto f=segmented_cube(cuts);std::string error;f.settings.level=1;
        float strength=1;for(auto e:f.selection.components)f.settings.edgeCreases[e]=strength++;
        auto before=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(before),error);
        EdgeBevelSettings settings;settings.width=.1F;settings.offsetType=BevelOffsetType::Absolute;settings.worldSpace=false;settings.autoFit=false;settings.segments=segments;settings.depth=segments==1?0.F:1.F;
        auto result=bevel_authored_editor_edges(f.mesh,f.selection,settings,{1,1,1},&f.settings);
        check(bool(result),"full split crease cuts="+std::to_string(cuts)+" segments="+std::to_string(segments)+": "+result.polygon.error);
        const auto diagnostic=diagnose_polygon_topology(result.polygon.mesh);check(!diagnostic.boundaries&&!diagnostic.nonmanifold&&!diagnostic.inconsistentWinding&&!diagnostic.degenerate,"segmented bevel is a closed valid solid");no_t_junctions(result.polygon.mesh,std::to_string(cuts)+"/"+std::to_string(segments));
        if(segments==1)check(std::abs(volume(result.polygon.mesh)-.99)<1e-5,"chain chamfer cuts once and retains single-crease volume");
        auto ids=advance_modeling_topology(f.mesh,result.polygon.mesh,&*result.subdivision,*before,result.lineage);check(bool(ids),ids.error);
        std::set<std::uint64_t> claimed;PolygonUvAuthoring flags;flags.layoutHash=polygon_uv_layout_hash(f.mesh);
        std::array<std::set<std::uint64_t>,2> expectedSeams;unsigned index=0;
        const auto seed=*f.selection.components.begin();const auto origin=f.mesh.vertices[std::uint32_t(seed>>32U)].position;
        const auto axis=normalize(subtract(f.mesh.vertices[std::uint32_t(seed)].position,origin));const auto parameter=[&](Float3 p){return dot(subtract(p,origin),axis);};
        std::vector<std::pair<float,float>> ranges;
        for(auto e:f.selection.components){
            const auto x=parameter(f.mesh.vertices[std::uint32_t(e>>32U)].position),y=parameter(f.mesh.vertices[std::uint32_t(e)].position);const auto lo=(std::min)(x,y),hi=(std::max)(x,y);ranges.emplace_back(lo,hi);
            const auto& descendants=result.lineage.edgeDescendants.at(e);check(!descendants.empty(),"each selected segment owns replacement rails");
            const auto a=before->vertices[std::uint32_t(e>>32U)],b=before->vertices[std::uint32_t(e)];
            const auto sourceEdge=before->edges.at(std::minmax(a,b));
            check(resolve_modeling_selection(result.polygon.mesh,ids.topology,std::uint8_t(PolygonSelectionMode::Edge),ids.descendants.at(sourceEdge))==descendants,"persistent segment selections resolve to their own replacement rails");
            for(auto next:descendants){
                check(claimed.insert(next).second,"different source segments never claim the same rail");check(result.subdivision->edgeCreases.at(next)==f.settings.edgeCreases.at(e),"segment-specific crease strength stays on its own rails");
                for(auto v:{std::uint32_t(next>>32U),std::uint32_t(next)}){const auto p=parameter(result.polygon.mesh.vertices[v].position);check(p>=lo-1e-5F&&p<=hi+1e-5F,"rail descendants stay inside their source segment interval");}
            }
            flags.seams[index%2].insert(e);expectedSeams[index%2].insert(descendants.begin(),descendants.end());++index;
        }
        for(auto t:result.polygon.selection.components){
            float lo=INFINITY,hi=-INFINITY;for(unsigned j=0;j<3;++j){const auto p=parameter(result.polygon.mesh.vertices[result.polygon.mesh.indices[t*3+j]].position);lo=(std::min)(lo,p);hi=(std::max)(hi,p);}
            check(std::any_of(ranges.begin(),ranges.end(),[&](auto r){return lo>=r.first-1e-5F&&hi<=r.second+1e-5F;}),"profile triangles respect original segment stations");
        }
        for(const auto& [old,next]:result.lineage.cornerDescendants)if(!next.empty()){flags.pins[0].insert(old);flags.pins[1].insert(old);}
        auto mapped=remap_polygon_uv_authoring(flags,result.polygon.mesh,result.lineage,&error);check(mapped&&mapped->seams==expectedSeams,"independent seam channels preserve per-segment flags: "+error);
        auto wider=settings;wider.width=.12F;auto changed=bevel_authored_editor_edges(f.mesh,f.selection,wider,{1,1,1},&f.settings);check(bool(changed),changed.polygon.error);
        auto repeated=advance_modeling_topology(f.mesh,changed.polygon.mesh,&*changed.subdivision,*before,changed.lineage);check(bool(repeated)&&ids.topology==repeated.topology,"stable chain connectivity retains provisional IDs when width changes");
        for(auto omitted:f.selection.components){auto partial=f.selection;partial.components.erase(omitted);auto rejected=bevel_authored_editor_edges(f.mesh,partial,settings,{1,1,1},&f.settings);
            check(!rejected&&rejected.polygon.error.find("every segment")!=std::string::npos&&polygon_asset_content_hash(rejected.polygon.mesh)==polygon_asset_content_hash(f.mesh)&&rejected.lineage.afterVertexGroups.empty(),"incomplete chain refuses atomically with an explicit reason");}
    }
    // Shear makes loop-cut face boundaries oblique to the crease's axial stations.
    // This exercises inserted rail points rather than only reusing existing cut vertices.
    auto f=segmented_cube(3);
    for(auto& v:f.mesh.vertices){const auto p=v.position;v.position={.13F+p.x+.2F*p.y+.1F*p.z,.27F+p.y+.15F*p.z,.39F+p.z};}
    refresh_editor_polygon(f.mesh);f.settings.level=1;
    EdgeBevelSettings settings;settings.offsetType=BevelOffsetType::Absolute;settings.autoFit=false;settings.width=.05F;settings.segments=4;
    const Float3 scale{1.7F,.6F,2.3F};auto actual=bevel_authored_editor_edges(f.mesh,f.selection,settings,scale,&f.settings);
    check(bool(actual),"sheared world chain: "+actual.polygon.error);no_t_junctions(actual.polygon.mesh,"world");
    auto scaled=f;for(auto& v:scaled.mesh.vertices)v.position={v.position.x*scale.x,v.position.y*scale.y,v.position.z*scale.z};refresh_editor_polygon(scaled.mesh);settings.worldSpace=false;
    auto reference=bevel_authored_editor_edges(scaled.mesh,scaled.selection,settings,{1,1,1},&scaled.settings);check(bool(reference),reference.polygon.error);
    check(actual.polygon.mesh.indices==reference.polygon.mesh.indices,"world chain keeps scaled-reference connectivity");
    for(std::size_t i=0;i<actual.polygon.mesh.vertices.size();++i){const auto& a=actual.polygon.mesh.vertices[i];const auto& b=reference.polygon.mesh.vertices[i];
        check(length_squared(subtract(Float3{a.position.x*scale.x,a.position.y*scale.y,a.position.z*scale.z},b.position))<1e-10F&&std::abs(a.texcoord.x-b.texcoord.x)<1e-5F&&std::abs(a.texcoord1.y-b.texcoord1.y)<1e-5F,"world chain keeps source-side UV interpolation");}
    auto plain=segmented_cube(2);auto ordinary=bevel_authored_editor_edges(plain.mesh,plain.selection,settings);
    check(bool(ordinary),"ordinary polygon segmented crease: "+ordinary.polygon.error);no_t_junctions(ordinary.polygon.mesh,"ordinary");
    // Interacting split chains need a separate corner-join reconstruction, and refuse atomically.
    std::map<std::uint64_t,std::vector<Float3>> normals;const auto groups=polygon_geometric_vertices(plain.mesh);
    for(std::uint32_t t=0;t<plain.mesh.indices.size()/3;++t){
        const auto a=plain.mesh.vertices[plain.mesh.indices[t*3]].position,b=plain.mesh.vertices[plain.mesh.indices[t*3+1]].position,c=plain.mesh.vertices[plain.mesh.indices[t*3+2]].position;
        const auto x=subtract(b,a),y=subtract(c,a);const auto n=normalize(Float3{x.y*y.z-x.z*y.y,x.z*y.x-x.x*y.z,x.x*y.y-x.y*y.x});
        for(unsigned j=0;j<3;++j)normals[key(groups[plain.mesh.indices[t*3+j]],groups[plain.mesh.indices[t*3+(j+1)%3]])].push_back(n);
    }
    auto multi=plain.selection;multi.components.clear();for(const auto& [e,n]:normals)if(n.size()==2&&dot(n[0],n[1])<.99F)multi.components.insert(e);
    auto corners=bevel_authored_editor_edges(plain.mesh,multi,settings,{1,1,1},&plain.settings);check(!corners&&corners.polygon.error.find("one split crease chain")!=std::string::npos&&polygon_asset_content_hash(corners.polygon.mesh)==polygon_asset_content_hash(plain.mesh)&&corners.lineage.afterVertexGroups.empty(),"meeting split chains refuse atomically with an explicit reason");
    // An uninterrupted crease meeting a split chain's endpoint stays supported on its own,
    // but combining it with that split chain refuses atomically without touching the source.
    {auto mixed=segmented_cube(1);mixed.settings.level=1;const auto mixedGroups=polygon_geometric_vertices(mixed.mesh);std::map<std::uint64_t,std::vector<Float3>> creaseNormals;
        for(std::uint32_t t=0;t<mixed.mesh.indices.size()/3;++t){const auto a=mixed.mesh.vertices[mixed.mesh.indices[t*3]].position,b=mixed.mesh.vertices[mixed.mesh.indices[t*3+1]].position,c=mixed.mesh.vertices[mixed.mesh.indices[t*3+2]].position;
            const auto x=subtract(b,a),y=subtract(c,a);const auto n=normalize(Float3{x.y*y.z-x.z*y.y,x.z*y.x-x.x*y.z,x.x*y.y-x.y*y.x});
            for(unsigned j=0;j<3;++j)creaseNormals[key(mixedGroups[mixed.mesh.indices[t*3+j]],mixedGroups[mixed.mesh.indices[t*3+(j+1)%3]])].push_back(n);}
        std::set<std::uint32_t> chainVertices;for(auto e:mixed.selection.components){chainVertices.insert(std::uint32_t(e>>32U));chainVertices.insert(std::uint32_t(e));}
        std::optional<std::uint64_t> other;for(const auto& [e,n]:creaseNormals)if(n.size()==2&&dot(n[0],n[1])<.99F&&!mixed.selection.components.contains(e)&&(chainVertices.contains(std::uint32_t(e>>32U))||chainVertices.contains(std::uint32_t(e)))){other=e;break;}
        check(bool(other),"segmented cube has an uninterrupted crease at a chain endpoint");
        auto single=mixed.selection;single.components={*other};auto alone=bevel_authored_editor_edges(mixed.mesh,single,settings,{1,1,1},&mixed.settings);check(bool(alone),"uninterrupted crease on a loop-cut cage remains supported: "+alone.polygon.error);
        auto combined=mixed.selection;combined.components.insert(*other);auto refused=bevel_authored_editor_edges(mixed.mesh,combined,settings,{1,1,1},&mixed.settings);
        check(!refused&&refused.polygon.error.find("one split crease chain")!=std::string::npos&&polygon_asset_content_hash(refused.polygon.mesh)==polygon_asset_content_hash(mixed.mesh)&&refused.lineage.afterVertexGroups.empty()&&!refused.subdivision,"split chain plus another crease refuses atomically");}
    // Materials may change along a chain, while the two sides of each segment agree.
    auto material=segmented_cube(2);material.mesh.materials.push_back(material.mesh.materials[0]);material.mesh.materials.back().name="Second interval";material.mesh.materialBindings.push_back(material.mesh.materialBindings[0]);
    const auto seed=*material.selection.components.begin();const auto origin=material.mesh.vertices[std::uint32_t(seed>>32U)].position;const auto axis=normalize(subtract(material.mesh.vertices[std::uint32_t(seed)].position,origin));
    const auto parameter=[&](Float3 p){return dot(subtract(p,origin),axis);};
    std::vector<std::pair<float,float>> intervals;
    for(auto e:material.selection.components){const auto a=parameter(material.mesh.vertices[std::uint32_t(e>>32U)].position),b=parameter(material.mesh.vertices[std::uint32_t(e)].position);intervals.emplace_back((std::min)(a,b),(std::max)(a,b));}
    std::sort(intervals.begin(),intervals.end());const auto border=intervals[0].second;
    material.mesh.submeshes.clear();
    for(std::uint32_t t=0;t<material.mesh.indices.size()/3;++t){float centre=0;for(unsigned j=0;j<3;++j)centre+=parameter(material.mesh.vertices[material.mesh.indices[t*3+j]].position)/3;
        material.mesh.submeshes.push_back({"Source interval",t*3,3,centre>border?1U:0U});}
    refresh_editor_polygon(material.mesh);auto colored=bevel_authored_editor_edges(material.mesh,material.selection,settings,{1,1,1},&material.settings);check(bool(colored),"segment materials: "+colored.polygon.error);
    for(const auto& sub:colored.polygon.mesh.submeshes)for(auto t=sub.firstIndex/3;t<(sub.firstIndex+sub.indexCount)/3;++t)if(colored.polygon.selection.components.contains(t)){
        float centre=0;for(unsigned j=0;j<3;++j)centre+=parameter(colored.polygon.mesh.vertices[colored.polygon.mesh.indices[t*3+j]].position)/3;
        check(sub.materialIndex==(centre>border?1U:0U),"profile slabs inherit their own source-segment material");}

}

void refusals_and_followup(){
    auto f=cube();EdgeBevelSettings settings;settings.width=.2F;std::string error;
    for(auto width:{0.F,-1.F,std::numeric_limits<float>::infinity()}){auto bad=settings;bad.width=width;auto result=bevel_authored_editor_edges(f.mesh,f.selection,bad,{1,1,1},&f.settings);check(!result&&polygon_asset_content_hash(result.polygon.mesh)==polygon_asset_content_hash(f.mesh)&&result.lineage.afterVertexGroups.empty(),"invalid width refuses atomically");}
    {auto tiny=settings;tiny.offsetType=BevelOffsetType::Absolute;tiny.autoFit=false;tiny.width=1e-6F;for(auto g:{cube(),fold(true,1),fold(false,1,true)}){auto r=bevel_authored_editor_edges(g.mesh,g.selection,tiny,{1,1,1},&g.settings);check(!r&&polygon_asset_content_hash(r.polygon.mesh)==polygon_asset_content_hash(g.mesh),"tiny width refuses instead of reporting a no-visible-change success");}}
    auto evaluatedSettings=f.settings;evaluatedSettings.level=1;auto evaluatedBevel=bevel_authored_editor_edges(f.mesh,f.selection,settings,{1,1,1},&evaluatedSettings);check(bool(evaluatedBevel),"active subdivision level validates before preview: "+evaluatedBevel.polygon.error);
    auto bad=settings;bad.offsetType=BevelOffsetType::Absolute;bad.worldSpace=true;check(!bevel_authored_editor_edges(f.mesh,f.selection,bad,{0,1,1},&f.settings),"invalid world scale refuses");
    const auto all=subdivision_control_edges(f.mesh,&f.settings);auto selected=f.selection;selected.components=all;
    auto multi=bevel_authored_editor_edges(f.mesh,selected,settings,{1,1,1},&f.settings);check(bool(multi),"multi-edge corner reconstruction: "+multi.polygon.error);
    auto split=cut_cage_edge_ring(f.mesh,f.settings,*all.begin(),.5F);check(bool(split),split.polygon.error);
    std::uint64_t unchanged=0;for(auto e:all)if(split.lineage.edgeDescendants.at(e).size()==1){unchanged=*split.lineage.edgeDescendants.at(e).begin();break;}check(unchanged!=0,"cut fixture has an uninterrupted crease");
    auto oldIds=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(oldIds),error);auto cutIds=advance_modeling_topology(f.mesh,split.polygon.mesh,&split.subdivision,*oldIds,split.lineage);check(bool(cutIds),cutIds.error);
    auto beveled=bevel_authored_editor_edges(split.polygon.mesh,{1,PolygonSelectionMode::Edge,{unchanged}},settings,{1,1,1},&split.subdivision);check(bool(beveled),"bevel after loop cut: "+beveled.polygon.error);
    auto mapped=advance_modeling_topology(split.polygon.mesh,beveled.polygon.mesh,&*beveled.subdivision,cutIds.topology,beveled.lineage,cutIds.topology.nextId+100);check(bool(mapped),mapped.error);
    for(auto id:mapped.created)check(id>=cutIds.topology.nextId+100,"new bevel does not reuse prior retired IDs");
    auto child=*split.lineage.edgeDescendants.at(*all.begin()).begin();check(!bevel_authored_editor_edges(split.polygon.mesh,{1,PolygonSelectionMode::Edge,{child}},settings,{1,1,1},&split.subdivision),"partial collinear crease refuses global trimming");
    for(auto ordinary:{cube(),fold(true,2),fold(false,1,true)}){auto edit=bevel_authored_editor_edges(ordinary.mesh,ordinary.selection,settings);check(bool(edit),"ordinary polygon bevel: "+edit.polygon.error);auto source=make_modeling_topology(ordinary.mesh,nullptr,{},&error);check(bool(source),error);auto ids=advance_modeling_topology(ordinary.mesh,edit.polygon.mesh,nullptr,*source,edit.lineage);check(bool(ids),"ordinary polygon identities: "+ids.error);}
    Fixture notch;notch.mesh.objectId=1;notch.mesh.materials.resize(1);notch.mesh.materialBindings.resize(1);notch.settings.level=0;
    for(unsigned layer=0;layer<2;++layer)for(auto point:{Float3{0,0,0},Float3{2,0,0},Float3{2,1,0},Float3{1,1,0},Float3{1,2,0},Float3{0,2,0}}){PolygonVertex vertex;vertex.position=add(point,{0,0,float(layer)});notch.mesh.vertices.push_back(vertex);}
    for(std::uint32_t j=0;j<6;++j){auto k=(j+1)%6;std::vector<std::uint32_t> wall{j,k,k+6,j,k+6,j+6};notch.mesh.indices.insert(notch.mesh.indices.end(),wall.begin(),wall.end());notch.settings.controlFaces.push_back(wall);}
    for(auto triangle:{std::array<std::uint32_t,3>{0,1,3},{1,2,3},{0,3,5},{3,4,5}}){std::vector<std::uint32_t> bottom{triangle[0],triangle[2],triangle[1]},top{triangle[0]+6,triangle[1]+6,triangle[2]+6};notch.mesh.indices.insert(notch.mesh.indices.end(),bottom.begin(),bottom.end());notch.mesh.indices.insert(notch.mesh.indices.end(),top.begin(),top.end());notch.settings.controlFaces.push_back(bottom);notch.settings.controlFaces.push_back(top);}
    notch.mesh.submeshes={{"Notch",0,std::uint32_t(notch.mesh.indices.size()),0}};refresh_editor_polygon(notch.mesh,true);check(!diagnose_polygon_topology(notch.mesh).boundaries,"notch fixture is closed");
    auto refused=bevel_authored_editor_edges(notch.mesh,{1,PolygonSelectionMode::Edge,{key(3,9)}},settings,{1,1,1},&notch.settings);check(!refused&&refused.polygon.error.find("Closed concave")!=std::string::npos,"closed concave corner joins refuse explicitly");
    auto simple=bevel_authored_editor_edges(f.mesh,f.selection,settings,{1,1,1},&f.settings);check(bool(simple),simple.polygon.error);
    PolygonUvAuthoring flags;flags.layoutHash=polygon_uv_layout_hash(f.mesh);
    auto surviving=std::find_if(simple.lineage.cornerDescendants.begin(),simple.lineage.cornerDescendants.end(),[](const auto& item){return !item.second.empty();});auto retired=std::find_if(simple.lineage.cornerDescendants.begin(),simple.lineage.cornerDescendants.end(),[](const auto& item){return item.second.empty();});
    check(surviving!=simple.lineage.cornerDescendants.end()&&retired!=simple.lineage.cornerDescendants.end(),"fixture has surviving and retired corners");flags.pins[1]={surviving->first};check(bool(remap_polygon_uv_authoring(flags,simple.polygon.mesh,simple.lineage,&error)),"surviving pins follow explicit corner descendants");flags.pins[0]={retired->first};check(!remap_polygon_uv_authoring(flags,simple.polygon.mesh,simple.lineage,&error),"retired pinned corners refuse remap");
    auto crease=f;crease.mesh.materials.push_back(crease.mesh.materials[0]);crease.mesh.materials[1].name="Other";crease.mesh.materialBindings.push_back(crease.mesh.materialBindings[0]);crease.mesh.submeshes={{"First",0,3,1},{"Rest",3,33,0}};refresh_editor_polygon(crease.mesh);
    check(!bevel_authored_editor_edges(crease.mesh,crease.selection,settings,{1,1,1},&crease.settings),"conflicting crease materials refuse");
    auto open=fold(false,1,true);open.mesh.vertices[1].texcoord1.x+=.7F;refresh_editor_polygon(open.mesh);check(!bevel_authored_editor_edges(open.mesh,open.selection,settings,{1,1,1},&open.settings),"non-affine second UV domain refuses");
    open=fold(false,1,true);open.mesh.vertices[1].position.x+=.3F;refresh_editor_polygon(open.mesh);check(!bevel_authored_editor_edges(open.mesh,open.selection,settings,{1,1,1},&open.settings),"skew endpoint rails refuse");
    auto bent=fold(true,2);for(auto& v:bent.mesh.vertices)if(v.position.x>1.5F)v.position.y+=.3F;refresh_editor_polygon(bent.mesh);check(!bevel_authored_editor_edges(bent.mesh,bent.selection,settings,{1,1,1},&bent.settings),"bent-chain joins refuse");
    for(auto& vertex:f.mesh.vertices)vertex.position=add(vertex.position,{.13F,.27F,.39F});refresh_editor_polygon(f.mesh);
    const Float3 scale{1.7F,.6F,2.3F};auto world=settings;world.offsetType=BevelOffsetType::Absolute;world.worldSpace=true;world.width=.1F;world.autoFit=false;
    auto actual=bevel_authored_editor_edges(f.mesh,f.selection,world,scale,&f.settings);check(bool(actual),actual.polygon.error);auto scaled=f.mesh;for(auto& v:scaled.vertices)v.position={v.position.x*scale.x,v.position.y*scale.y,v.position.z*scale.z};refresh_editor_polygon(scaled);world.worldSpace=false;
    auto reference=bevel_authored_editor_edges(scaled,f.selection,world,{1,1,1},&f.settings);check(bool(reference),reference.polygon.error);check(actual.polygon.mesh.indices==reference.polygon.mesh.indices,"world width preserves reference connectivity");
    for(std::size_t i=0;i<reference.polygon.mesh.vertices.size();++i){auto p=reference.polygon.mesh.vertices[i].position;p={p.x/scale.x,p.y/scale.y,p.z/scale.z};check(length_squared(subtract(p,actual.polygon.mesh.vertices[i].position))<1e-10F,"world width handles nonuniform scale");}
}
#if defined(DVE_EDGE_BEVEL_COMMANDS) || !defined(DVE_EDGE_BEVEL_CORE_ONLY)
void history(){for(auto f:{cube(),fold(true,2),fold(false,1,true),segmented_cube(1),segmented_cube(3)})for(bool identified:{false,true}){
    std::string error;EditorDocument document("Bevel history");EditorObject object(1,"Source");object.polygon=f.mesh;object.subdivision=f.settings;if(identified)check(ensure_modeling_topology(object,&error),error);const auto before=object.modelingTopology;document.add_object(std::move(object));
    EdgeBevelSettings settings;auto result=bevel_authored_editor_edges(f.mesh,f.selection,settings,{1,1,1},&f.settings);check(bool(result),result.polygon.error);auto source=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(source),error);auto ids=advance_modeling_topology(f.mesh,result.polygon.mesh,&*result.subdivision,identified?*before:*source,result.lineage);check(bool(ids),ids.error);
    PolygonUvAuthoring flags;flags.layoutHash=polygon_uv_layout_hash(f.mesh);flags.seams[0]=f.selection.components;flags.seams[1]=f.selection.components;auto save=make_polygon_uv_authoring_command(document,*document.find_object(1),flags,&error);check(save&&save->execute(document).success,"save bevel flags");
    auto record=remap_polygon_uv_authoring_record(*document.find_object(1),result.polygon.mesh,result.lineage,&error);check(bool(record),error);const auto* component=find_component_by_type(std::span<const Component>(document.find_object(1)->components),kUvAuthoringComponent);
    auto command=std::make_unique<CompoundCommand>("Authored bevel");command->add(std::make_unique<ReplacePolygonCommand>(1,f.mesh,result.polygon.mesh,"Bevel",0,result.subdivision,false,ids.topology));command->add(std::make_unique<SetComponentPropertyCommand>(1,component->id,"state",component->properties.at("state"),ComponentValue{*record},"Preserve flags"));EditorCommandStack stack;
    check(stack.execute(document,std::move(command)).success,"actual compound bevel Apply");const auto floor=document.find_object(1)->modelingNextId;auto afterFlags=read_polygon_uv_authoring(*document.find_object(1),&error);check(afterFlags&&afterFlags->seams[0]==afterFlags->seams[1]&&document.find_object(1)->modelingTopology==ids.topology,"Apply stores prepared IDs and both flag channels");
    check(stack.undo(document).success&&document.find_object(1)->modelingTopology==before&&document.find_object(1)->modelingNextId>=floor&&polygon_asset_content_hash(*document.find_object(1)->polygon)==polygon_asset_content_hash(f.mesh),"Undo restores exact legacy/identified source while retaining issued floor");
    check(stack.redo(document).success&&document.find_object(1)->modelingTopology==ids.topology&&polygon_asset_content_hash(*document.find_object(1)->polygon)==polygon_asset_content_hash(result.polygon.mesh),"Redo restores exact bevel and IDs");
    if((f.mesh.indices.size()==36||f.selection.components.size()>1)&&!identified){const auto root=std::filesystem::temp_directory_path()/"dve-authored-bevel-roundtrip";std::filesystem::remove_all(root);const auto path=root/"bevel.dvescene";const auto saved=document.save_transactional(path);check(saved.success,saved.error);auto loaded=EditorDocument::load(path,&error);check(loaded&&loaded->find_object(1)->modelingTopology==ids.topology&&polygon_asset_content_hash(*loaded->find_object(1)->polygon)==polygon_asset_content_hash(result.polygon.mesh),"save/reopen retains the actual beveled mesh and prepared identity record");auto flagsAfter=read_polygon_uv_authoring(*loaded->find_object(1),&error);check(flagsAfter&&flagsAfter->seams==afterFlags->seams,"save/reopen retains bevel seam records");std::filesystem::remove_all(root);}
}}
#endif
#ifndef DVE_EDGE_BEVEL_CORE_ONLY
void native(){for(auto f:{cube(),fold(true,2),fold(false,1,true),segmented_cube(1),segmented_cube(3)})for(bool identified:{false,true}){
    EditorDocument document("Native bevel");EditorObject object(1,"Source");object.polygon=f.mesh;object.subdivision=f.settings;std::string error;if(identified)check(ensure_modeling_topology(object,&error),error);const auto before=object.modelingTopology;const auto floor=object.modelingNextId;document.add_object(std::move(object));
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));auto& c=*owner;c.resize(1600,900);c.workspace().select_object(1);check(c.dispatch_action("subdiv.edge_mode"),"native edge mode");c.select_polygon_components(f.selection.components,SelectionOperation::Replace);const auto count=c.workspace().commands().size();
    {const auto* menu=c.workspace().menus().find("mesh.chamfer_edges");check(menu&&menu->enabled,"bevel menu enabled for selected edges");}
    {EditorObject other(2,"Other");other.polygon=f.mesh;c.workspace().document().add_object(std::move(other));c.workspace().select_object(2);c.refresh_menu_state();const auto* menu=c.workspace().menus().find("mesh.chamfer_edges");check(menu&&!menu->enabled&&!menu->disabledReason.empty(),"bevel menu disabled with a reason when the edge selection belongs to another object");check(!c.dispatch_action("mesh.chamfer_edges"),"bevel refuses foreign edge selection");c.workspace().select_object(1);c.select_polygon_components(f.selection.components,SelectionOperation::Replace);}
    check(c.dispatch_action("mesh.chamfer_edges"),"native authored bevel preview");c.key_down("escape",false,false,false);auto* target=c.workspace().document().find_object(1);check(target->modelingTopology==before&&target->modelingNextId==floor&&c.workspace().commands().size()==count,"Cancel leaves no migration, reservation or history");
    check(c.dispatch_action("mesh.chamfer_edges"),"source-change guard preview");for(auto& vertex:target->polygon->vertices)vertex.position.x+=.1F;refresh_editor_polygon(*target->polygon);const auto editedSource=polygon_asset_content_hash(*target->polygon);c.key_down("enter",false,false,false);check(c.workspace().commands().size()==count&&polygon_asset_content_hash(*target->polygon)==editedSource,"source changes refuse Apply without rebinding array indices");c.key_down("escape",false,false,false);*target->polygon=f.mesh;
    check(c.dispatch_action("mesh.fillet_edges"),"native rounded bevel preview");c.key_down("enter",false,false,false);check(c.workspace().commands().size()==count+1&&target->modelingTopology.has_value(),"Enter applies one identity-aware bevel transaction");const auto output=*target->modelingTopology;const auto hash=polygon_asset_content_hash(*target->polygon);
    check(c.dispatch_action("edit.undo")&&target->modelingTopology==before&&c.polygon_selection().components==f.selection.components,"native Undo restores source and edge selection");check(c.dispatch_action("edit.redo")&&target->modelingTopology==output&&polygon_asset_content_hash(*target->polygon)==hash,"native Redo restores exact geometry and IDs");
}}
#endif
}
int main(){try{run(cube(),true);run(fold(false),false);run(fold(true),false);run(fold(true,2),false);run(fold(false,1,true),false);run(fold(false,2,true),false);refusals_and_followup();split_crease_chains();
#if defined(DVE_EDGE_BEVEL_COMMANDS) || !defined(DVE_EDGE_BEVEL_CORE_ONLY)
history();
#endif
#ifndef DVE_EDGE_BEVEL_CORE_ONLY
native();
#endif
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Authored edge bevel regression checks passed\n";}
