#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_modeling_topology.hpp"
#include "dve/editor_subdivision.hpp"
#include "dve/voxel_metric.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <iterator>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <utility>

namespace dve::editor {
namespace {
using Index=std::uint32_t;using Edge=std::uint64_t;
Edge edge(Index a,Index b){return (Edge((std::min)(a,b))<<32U)|(std::max)(a,b);}
Float3 cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
struct Vertex {
    PolygonVertex attribute;Index group{};std::optional<Index> original,corner,geometricParent;
    std::set<Index> attributeParents;
};
struct Patch {
    std::vector<Vertex> vertices;std::optional<Index> triangle,face;
    std::optional<Edge> bevelEdge;Index material{};
};
struct Half {Index a,b,va,vb,triangle,face;Float3 normal;};
PolygonVertex mix(const PolygonVertex& a,const PolygonVertex& b,float t){
    auto out=a;const auto f=[&](float x,float y){return x+(y-x)*t;};
    out.position=add(a.position,multiply(subtract(b.position,a.position),t));
    out.normal=normalize(add(a.normal,multiply(subtract(b.normal,a.normal),t)));
    out.texcoord={f(a.texcoord.x,b.texcoord.x),f(a.texcoord.y,b.texcoord.y)};out.texcoord1={f(a.texcoord1.x,b.texcoord1.x),f(a.texcoord1.y,b.texcoord1.y)};
    out.color={f(a.color.x,b.color.x),f(a.color.y,b.color.y),f(a.color.z,b.color.z),f(a.color.w,b.color.w)};
    out.tangent={f(a.tangent.x,b.tangent.x),f(a.tangent.y,b.tangent.y),f(a.tangent.z,b.tangent.z),f(a.tangent.w,b.tangent.w)};return out;
}
Vertex interpolate(const Vertex& a,const Vertex& b,float t,Index group,Float3 position){
    Vertex out;out.attribute=mix(a.attribute,b.attribute,t);out.attribute.position=position;out.group=group;
    out.attributeParents=a.attributeParents;out.attributeParents.insert(b.attributeParents.begin(),b.attributeParents.end());return out;
}
Float3 patch_normal(const Patch& patch){Float3 area{};const auto origin=patch.vertices[0].attribute.position;for(std::size_t j=0;j<patch.vertices.size();++j)area=add(area,cross(subtract(patch.vertices[j].attribute.position,origin),subtract(patch.vertices[(j+1)%patch.vertices.size()].attribute.position,origin)));return normalize(area);}
std::vector<Float3> profile_points(Float3 d0,Float3 d1,float setback,const EdgeBevelSettings& settings){
    const auto first=multiply(d0,setback),last=multiply(d1,setback);
    const auto cosine=std::clamp(dot(d0,d1),-1.F,1.F);if(cosine<-.9999F||cosine>.9999F)throw std::runtime_error("Bevel faces are coplanar or opposed");
    const auto centre=multiply(add(d0,d1),setback/(1+cosine));const auto r0=subtract(first,centre),r1=subtract(last,centre);
    const auto angle=std::acos(std::clamp(dot(normalize(r0),normalize(r1)),-1.F,1.F));const auto count=settings.depth==0?1U:settings.segments;
    std::vector<Float3> points;
    for(Index i=0;i<=count;++i){const auto t=float(i)/float(count);const auto flat=add(first,multiply(subtract(last,first),t));
        const auto arc=add(centre,multiply(add(multiply(r0,std::sin((1-t)*angle)),multiply(r1,std::sin(t*angle))),1/std::sin(angle)));
        points.push_back(add(flat,multiply(subtract(arc,flat),settings.depth)));}
    points.front()=first;points.back()=last;return points;
}
}

PolygonFaceEditResult bevel_authored_editor_edges(const CookedPolygonAsset& source,const PolygonSelection& selection,
    const EdgeBevelSettings& settings,Float3 objectScale,const SubdivisionSettings* authored) {
    PolygonFaceEditResult result;result.polygon={source,selection,{}};
    const auto fail=[&](std::string error){result.polygon={source,selection,std::move(error)};result.lineage={};result.subdivision.reset();return result;};
    if(!validate_polygon_asset(source)||!polygon_selection_valid(source,selection)||selection.mode!=PolygonSelectionMode::Edge||selection.components.empty())return fail("Select valid authored edges for bevel");
    if(!std::isfinite(settings.width)||settings.width<=0||(settings.offsetType==BevelOffsetType::Fractional&&settings.width>1)||
        (settings.offsetType!=BevelOffsetType::Fractional&&settings.offsetType!=BevelOffsetType::Absolute)||settings.segments<1||settings.segments>16||
        !std::isfinite(settings.depth)||settings.depth<0||settings.depth>1||!std::isfinite(settings.smoothingAngle)||settings.smoothingAngle<0||settings.smoothingAngle>180)
        return fail("Width must be positive (fraction <=1), Segments 1..16, Depth 0..1 and Smoothing Angle 0..180");
    if(source.vertices.size()>100000||source.indices.size()>150000||selection.components.size()>256/settings.segments)return fail("Bevel exceeds its modeling budget: 256 profile segments, 100000 attributes, 50000 triangles");
    const bool world=settings.offsetType==BevelOffsetType::Absolute&&settings.worldSpace;
    if(world&&(!std::isfinite(objectScale.x)||!std::isfinite(objectScale.y)||!std::isfinite(objectScale.z)||objectScale.x<=1e-8F||objectScale.y<=1e-8F||objectScale.z<=1e-8F))return fail("World width needs finite positive object scale");
    const auto diagnostic=diagnose_polygon_topology(source);
    if(diagnostic.nonmanifold||diagnostic.degenerate||diagnostic.inconsistentWinding)return fail("Bevel needs consistently wound manifold input");
    std::string error;if(!make_modeling_topology(source,authored,{},&error))return fail(error);
    try {
        auto input=source;if(world)for(auto& v:input.vertices)v.position=multiply_components(v.position,objectScale);
        const auto canonical=polygon_geometric_vertices(source);std::vector<Index> materials(source.indices.size()/3);
        for(const auto& sub:source.submeshes)for(Index t=sub.firstIndex/3;t<(sub.firstIndex+sub.indexCount)/3;++t)materials[t]=sub.materialIndex;
        std::vector<std::vector<Index>> faces;
        if(authored)faces=subdivision_control_face_triangles(source,authored);else for(Index t=0;t<materials.size();++t)faces.push_back({t});
        // Even an ordinary mesh can use adjacent compatible render triangles as planar face patches.
        SubdivisionSettings automatic;
        if(!authored){freeze_subdivision_topology(source,automatic);std::vector<std::vector<Index>> planar;
            for(const auto& record:automatic.controlFaces){if(record.size()==6){const auto n=[&](Index j){const auto a=input.vertices[record[j]].position,b=input.vertices[record[j+1]].position,c=input.vertices[record[j+2]].position;return normalize(cross(subtract(b,a),subtract(c,a)));};
                    if(dot(n(0),n(3))<.99999F){planar.push_back({record[0],record[1],record[2]});planar.push_back({record[3],record[4],record[5]});continue;}}
                planar.push_back(record);}automatic.controlFaces=std::move(planar);faces=subdivision_control_face_triangles(source,&automatic);}
        std::vector<Index> owner(materials.size());for(Index f=0;f<faces.size();++f)for(auto t:faces[f])owner[t]=f;
        std::map<Edge,std::vector<Half>> halves;std::map<Index,std::set<Edge>> tags;std::map<Index,Float3> positions;
        for(Index i=0;i<input.vertices.size();++i)positions[canonical[i]]=input.vertices[i].position;
        float extent=0;const auto origin=input.vertices[input.indices[0]].position;
        for(auto index:input.indices){extent=(std::max)(extent,length_squared(subtract(input.vertices[index].position,origin)));if(!std::isfinite(extent))return fail("Bevel coordinates overflow");}
        const auto tolerance=std::sqrt(extent)*1e-6F;
        std::vector<Patch> patches;
        for(Index t=0;t<materials.size();++t){Patch p;p.triangle=t;p.face=owner[t];p.material=materials[t];
            for(Index j=0;j<3;++j){const auto a=input.indices[t*3+j],b=input.indices[t*3+(j+1)%3];Vertex v;v.attribute=input.vertices[a];v.original=a;v.corner=t*3+j;v.group=canonical[a];v.attributeParents={a};p.vertices.push_back(v);const auto e=edge(canonical[a],canonical[b]);tags[canonical[a]].insert(e);tags[canonical[b]].insert(e);}
            const auto n=patch_normal(p);for(Index j=0;j<3;++j){const auto a=input.indices[t*3+j],b=input.indices[t*3+(j+1)%3];halves[edge(canonical[a],canonical[b])].push_back({canonical[a],canonical[b],a,b,t,owner[t],n});}patches.push_back(std::move(p));}
        const auto authoredEdges=subdivision_control_edges(source,authored?authored:&automatic);
        for(auto selected:selection.components)if(!authoredEdges.contains(selected))return fail("Select face-boundary edges, not hidden render diagonals");
        float shortest=INFINITY,minSine=1;
        for(auto selected:selection.components){const auto& h=halves.at(selected);if(h.size()==2&&dot(h[0].normal,h[1].normal)>.99999F)return fail("Select a crease or open boundary, not a coplanar edge");
            if(h.size()==2)minSine=(std::min)(minSine,std::sqrt((1-std::clamp(dot(h[0].normal,h[1].normal),-1.F,1.F))*.5F));}
        for(auto e:authoredEdges)shortest=(std::min)(shortest,length(subtract(positions.at(Index(e>>32U)),positions.at(Index(e)))));
        const auto safe=shortest*.49F*minSine;if(!std::isfinite(safe)||safe<=0)return fail("Cannot determine a safe bevel width");
        float width=settings.offsetType==BevelOffsetType::Fractional?settings.width*safe:settings.width;if(settings.autoFit)width=(std::min)(width,safe);
        if(width<=tolerance*8)return fail("Bevel width is too small to change this mesh");
        bool convex=diagnostic.boundaries==0;
        if(convex){std::set<std::tuple<float,float,float,float>> planes;
            for(const auto& patch:patches){const auto n=patch_normal(patch);planes.emplace(n.x,n.y,n.z,dot(n,patch.vertices[0].attribute.position));}
            if(planes.size()*tags.size()>2000000)return fail("Convexity check exceeds its two-million-incidence budget; simplify or split the mesh");
            for(const auto& [x,y,z,d]:planes){for(const auto& [g,v]:positions)if(tags.contains(g)&&dot(Float3{x,y,z},v)-d>tolerance){convex=false;break;}if(!convex)break;}
        }
        Index nextGroup=Index(source.vertices.size());
        std::map<Edge,Index> selectedMaterial;
        const auto material_hash=[&](Index material){CookedPolygonAsset m;m.materials={source.materials[material]};m.materialBindings={source.materialBindings[material]};return polygon_asset_content_hash(m);};
        for(auto e:selection.components){const auto& h=halves.at(e);selectedMaterial[e]=materials[h[0].triangle];if(h.size()==2&&material_hash(materials[h[0].triangle])!=material_hash(materials[h[1].triangle]))return fail("Resolve conflicting materials on the selected crease before beveling");}
        if(convex){
            struct CreaseChain {std::vector<Edge> edges;std::vector<Index> vertices;};
            std::vector<CreaseChain> chains;std::set<Edge> grouped;
            for(auto seed:selection.components)if(!grouped.contains(seed)){
                const auto& reference=halves.at(seed);
                const auto direction=normalize(subtract(positions.at(reference[0].b),positions.at(reference[0].a)));
                const auto base=positions.at(reference[0].a);
                std::set<Edge> component;std::queue<Edge> pending;pending.push(seed);
                std::map<Index,std::set<Edge>> incident;
                while(!pending.empty()){
                    const auto selected=pending.front();pending.pop();if(!component.insert(selected).second)continue;
                    for(auto endpoint:{Index(selected>>32U),Index(selected)}){
                        incident[endpoint].insert(selected);
                        for(auto next:tags.at(endpoint))if(next!=selected&&authoredEdges.contains(next)){
                            const auto& adjacent=halves.at(next);
                            if(adjacent.size()!=2||dot(adjacent[0].normal,adjacent[1].normal)>.99999F)continue;
                            const auto tangent=normalize(subtract(positions.at(Index(next)),positions.at(Index(next>>32U))));
                            if(std::abs(dot(direction,tangent))<=.99999F)continue;
                            if(!selection.components.contains(next))return fail("Select every segment of a straight split crease; partial-chain endpoint joins are not supported");
                            for(auto v:{Index(next>>32U),Index(next)})if(length(cross(subtract(positions.at(v),base),direction))>tolerance*2)
                                return fail("Split crease is bent; straight-chain bevel requires one supporting line");
                            const auto matches=[&](Index i,Index j){return dot(reference[i].normal,adjacent[j].normal)>.99999F;};
                            if(!((matches(0,0)&&matches(1,1))||(matches(0,1)&&matches(1,0))))return fail("Straight split crease changes face planes; bevel its endpoint joins separately");
                            if(!component.contains(next))pending.push(next);
                        }
                    }
                }
                std::vector<Index> ends;
                for(const auto& [v,edges]:incident){if(edges.size()>2)return fail("Straight crease chains cannot branch");if(edges.size()==1)ends.push_back(v);}
                if(ends.size()!=2)return fail("Straight crease chain needs two distinct endpoints");
                CreaseChain chain;auto current=ends.front();chain.vertices.push_back(current);
                while(chain.edges.size()<component.size()){
                    std::optional<Edge> next;for(auto e:incident.at(current))if(chain.edges.empty()||e!=chain.edges.back()){next=e;break;}
                    if(!next)return fail("Straight crease chain lost its continuation");
                    chain.edges.push_back(*next);current=Index(*next>>32U)==current?Index(*next):Index(*next>>32U);chain.vertices.push_back(current);
                }
                grouped.insert(component.begin(),component.end());chains.push_back(std::move(chain));
            }
            if(chains.size()>1&&std::any_of(chains.begin(),chains.end(),[](const auto& chain){return chain.edges.size()>1;}))
                return fail("Bevel one split crease chain at a time; joins between multiple crease chains need separate corner reconstruction");
            // Each physical crease is cut once; selected segments label separate axial slabs.
            // Symbolic edge/plane intersections share geometry without matching old positions.
            for(const auto& chain:chains){const auto selected=chain.edges.front();const auto& h=halves.at(selected);const auto n0=h[0].normal,n1=h[1].normal;
                const auto axis=normalize(subtract(positions.at(h[0].b),positions.at(h[0].a)));const auto d0=normalize(cross(n0,axis)),d1=normalize(cross(n1,multiply(axis,-1)));
                const auto sine=std::sqrt((1-std::clamp(dot(n0,n1),-1.F,1.F))*.5F);const auto profile=profile_points(d0,d1,width/sine,settings);const auto base=positions.at(h[0].a);
                for(Index step=1;step<profile.size();++step){auto normal=normalize(cross(axis,subtract(profile[step],profile[step-1])));if(dot(normal,add(n0,n1))<0)normal=multiply(normal,-1);
                    const auto distance=dot(normal,add(base,profile[step]));std::map<Edge,Index> intersections;std::vector<Patch> clipped;bool removed=false;
                    for(const auto& patch:patches){Patch out=patch;out.vertices.clear();
                        for(std::size_t j=0;j<patch.vertices.size();++j){const auto& a=patch.vertices[j];const auto& b=patch.vertices[(j+1)%patch.vertices.size()];const auto da=dot(normal,a.attribute.position)-distance,db=dot(normal,b.attribute.position)-distance;const bool insideA=da<=tolerance,insideB=db<=tolerance;
                            if(insideA)out.vertices.push_back(a);else removed=true;
                            if(insideA!=insideB){auto fraction=std::clamp(da/(da-db),0.F,1.F);const auto key=edge(a.group,b.group);Index group;
                                // Adjacent profile planes can meet exactly on a render diagonal.
                                // Reuse an endpoint already on the plane instead of making a near-duplicate.
                                if(std::abs(da)<=tolerance||fraction<=1e-6F){fraction=0;group=a.group;}else if(std::abs(db)<=tolerance||fraction>=1-1e-6F){fraction=1;group=b.group;}else {auto [it,created]=intersections.emplace(key,nextGroup);if(created){group=nextGroup++;positions[group]=add(a.attribute.position,multiply(subtract(b.attribute.position,a.attribute.position),fraction));std::set_intersection(tags[a.group].begin(),tags[a.group].end(),tags[b.group].begin(),tags[b.group].end(),std::inserter(tags[group],tags[group].end()));}group=it->second;}
                                auto intersection=fraction==0?a:fraction==1?b:interpolate(a,b,fraction,group,positions.at(group));const auto& retired=insideA?b:a;if(retired.geometricParent)intersection.geometricParent=retired.geometricParent;else if(retired.original)intersection.geometricParent=canonical[*retired.original];out.vertices.push_back(std::move(intersection));}}
                        std::vector<Vertex> unique;for(const auto& v:out.vertices)if(unique.empty()||unique.back().group!=v.group)unique.push_back(v);if(unique.size()>1&&unique.front().group==unique.back().group)unique.pop_back();out.vertices=std::move(unique);if(out.vertices.size()>=3)clipped.push_back(std::move(out));}
                    if(!removed)return fail("Bevel profile did not cut the source; increase width");
                    std::map<Edge,std::vector<std::pair<Vertex,Vertex>>> cut;
                    for(const auto& patch:clipped)for(std::size_t j=0;j<patch.vertices.size();++j){auto a=patch.vertices[j],b=patch.vertices[(j+1)%patch.vertices.size()];if(std::abs(dot(normal,a.attribute.position)-distance)<=tolerance*2&&std::abs(dot(normal,b.attribute.position)-distance)<=tolerance*2)cut[edge(a.group,b.group)].push_back({b,a});}
                    std::map<Index,std::pair<Vertex,Vertex>> next;
                    for(const auto& [e,segments]:cut)if(segments.size()==1){if(!next.emplace(segments[0].first.group,segments[0]).second)return fail("Bevel cut branches at a corner");}else if(segments.size()!=2)return fail("Bevel cut has ambiguous incidence");
                    if(next.empty())return fail("Bevel cut lost its boundary");Patch cap;cap.bevelEdge=selected;cap.material=selectedMaterial.at(selected);const auto start=next.begin()->first;auto current=start;
                    do{auto at=next.find(current);if(at==next.end())return fail("Bevel cut has an open corner join");auto v=at->second.first;current=at->second.second.group;v.original.reset();v.corner.reset();v.attribute.normal=world?normalize(multiply_components(normal,objectScale)):normal;cap.vertices.push_back(v);next.erase(at);}while(current!=start&&cap.vertices.size()<=4096);
                    if(!next.empty()||current!=start||cap.vertices.size()<3||cap.vertices.size()>512)return fail("Bevel supports one convex cut contour of at most 512 vertices per profile plane");
                    if(chain.edges.size()==1)clipped.push_back(std::move(cap));
                    else {
                        const auto chainOrigin=positions.at(chain.vertices.front());
                        const auto chainAxis=normalize(subtract(positions.at(chain.vertices.back()),chainOrigin));
                        struct BoundarySplit {Index group,parent;float fraction;};
                        std::map<Edge,std::vector<BoundarySplit>> boundarySplits;
                        const auto spliceBoundary=[&](){
                            // A slab boundary may split a rail between existing cage vertices.
                            // Insert the same geometric groups into its neighboring source patch,
                            // interpolating that patch's own fields rather than sharing attributes.
                            for(auto& patch:clipped){
                                std::vector<Vertex> split;
                                for(std::size_t j=0;j<patch.vertices.size();++j){
                                    const auto& a=patch.vertices[j];const auto& b=patch.vertices[(j+1)%patch.vertices.size()];split.push_back(a);
                                    if(auto at=boundarySplits.find(edge(a.group,b.group));at!=boundarySplits.end()){
                                        auto points=at->second;std::sort(points.begin(),points.end(),[&](const auto& x,const auto& y){return a.group<b.group?x.fraction<y.fraction:x.fraction>y.fraction;});
                                        for(const auto& point:points){const auto fraction=a.group<b.group?point.fraction:1-point.fraction;
                                            auto v=interpolate(a,b,fraction,point.group,positions.at(point.group));v.geometricParent=point.parent;split.push_back(std::move(v));}
                                    }
                                }
                                patch.vertices=std::move(split);
                            }
                        };
                        std::vector<Patch> strips;
                        auto remaining=std::move(cap);
                        for(std::size_t part=0;part+1<chain.edges.size();++part){
                            const auto joint=chain.vertices[part+1];const auto station=dot(subtract(positions.at(joint),chainOrigin),chainAxis);
                            Patch lower=remaining,upper=remaining;lower.vertices.clear();upper.vertices.clear();
                            for(std::size_t j=0;j<remaining.vertices.size();++j){
                                const auto& a=remaining.vertices[j];const auto& b=remaining.vertices[(j+1)%remaining.vertices.size()];
                                const auto da=dot(subtract(a.attribute.position,chainOrigin),chainAxis)-station;
                                const auto db=dot(subtract(b.attribute.position,chainOrigin),chainAxis)-station;
                                if(da<=tolerance)lower.vertices.push_back(a);if(da>=-tolerance)upper.vertices.push_back(a);
                                if((da < -tolerance&&db > tolerance)||(db < -tolerance&&da > tolerance)){
                                    const auto fraction=da/(da-db);const auto group=nextGroup++;
                                    const auto point=add(a.attribute.position,multiply(subtract(b.attribute.position,a.attribute.position),fraction));positions[group]=point;
                                    std::set_intersection(tags[a.group].begin(),tags[a.group].end(),tags[b.group].begin(),tags[b.group].end(),std::inserter(tags[group],tags[group].end()));
                                    auto v=interpolate(a,b,fraction,group,point);v.geometricParent=joint;
                                    lower.vertices.push_back(v);upper.vertices.push_back(std::move(v));
                                    boundarySplits[edge(a.group,b.group)].push_back({group,joint,a.group<b.group?fraction:1-fraction});
                                }
                            }
                            if(lower.vertices.size()>=3){lower.bevelEdge=chain.edges[part];lower.material=selectedMaterial.at(chain.edges[part]);strips.push_back(std::move(lower));}
                            spliceBoundary();boundarySplits.clear();remaining=std::move(upper);
                        }
                        if(remaining.vertices.size()>=3){remaining.bevelEdge=chain.edges.back();remaining.material=selectedMaterial.at(chain.edges.back());strips.push_back(std::move(remaining));}
                        clipped.insert(clipped.end(),std::make_move_iterator(strips.begin()),std::make_move_iterator(strips.end()));
                    }
                    patches=std::move(clipped);
                    if(patches.size()>50000||nextGroup>150000)return fail("Bevel reconstruction exceeds its modeling budget");
                }
            }
        }else {
            if(!diagnostic.boundaries)return fail("Closed concave endpoint joins are not supported; use a straight open crease chain");
            // Open reconstruction operates on planar affine quad domains, with explicit open ends.
            std::map<Index,std::set<Edge>> selectedAt;std::set<Index> touched;
            for(auto e:selection.components){const auto& h=halves.at(e);selectedAt[Index(e>>32U)].insert(e);selectedAt[Index(e)].insert(e);for(const auto& half:h)touched.insert(half.face);}
            const auto boundary=halves.at(*selection.components.begin()).size()==1;
            for(auto e:selection.components)if((halves.at(e).size()==1)!=boundary)return fail("Do not mix boundary rolls and two-face crease bevels");
            Index endpoints=0;for(const auto& [v,edges]:selectedAt){if(edges.size()>2)return fail("Bevel chains cannot branch");if(edges.size()==1)++endpoints;}
            if(endpoints!=2)return fail("Open bevel needs one unbranched chain with two open ends");
            std::set<Edge> visited;std::queue<Edge> queue;queue.push(*selection.components.begin());while(!queue.empty()){auto e=queue.front();queue.pop();if(!visited.insert(e).second)continue;for(auto v:{Index(e>>32U),Index(e)})for(auto next:selectedAt[v])queue.push(next);}if(visited!=selection.components)return fail("Select one connected open chain");
            const auto& first=halves.at(*selection.components.begin()).front();const auto axis=normalize(subtract(positions.at(first.b),positions.at(first.a)));
            for(auto e:selection.components){const auto direction=normalize(subtract(positions.at(Index(e)),positions.at(Index(e>>32U))));if(std::abs(dot(axis,direction))<.99999F)return fail("Initial open bevel supports straight chains only; bent-chain miters are separate work");}
            for(const auto& [v,edges]:selectedAt)for(auto e:tags.at(v))for(const auto& half:halves.at(e))if(!touched.contains(half.face))return fail("Bevel chain endpoints or corners meet unsupported additional faces");
            std::map<Index,std::vector<Vertex>> quads;std::map<Index,Edge> faceEdge;
            for(auto e:selection.components)for(const auto& half:halves.at(e)){if(!faceEdge.emplace(half.face,e).second)return fail("A face cannot contain multiple selected bevel edges");}
            for(auto f:touched){if(faces[f].size()!=2)return fail("Open bevel requires explicit planar quad face domains");std::map<Index,Vertex> attributes;std::map<Edge,std::vector<std::pair<Index,Index>>> faceEdges;
                for(auto t:faces[f])for(Index j=0;j<3;++j){const auto a=source.indices[t*3+j],b=source.indices[t*3+(j+1)%3];Vertex v;v.attribute=input.vertices[a];v.original=a;v.group=canonical[a];v.attributeParents={a};if(auto at=attributes.find(v.group);at!=attributes.end()&&at->second.original!=v.original)return fail("Resolve internal split attributes before local open bevel");attributes.emplace(v.group,v);faceEdges[edge(canonical[a],canonical[b])].push_back({canonical[a],canonical[b]});}
                std::map<Index,Index> next;for(const auto& [e,h]:faceEdges)if(h.size()==1)next[h[0].first]=h[0].second;if(next.size()!=4)return fail("Open bevel requires simple quad boundaries");auto start=next.begin()->first,current=start;do{quads[f].push_back(attributes.at(current));current=next.at(current);}while(current!=start&&quads[f].size()<5);if(quads[f].size()!=4)return fail("Invalid open face domain");
                const auto& q=quads[f];const auto n=normalize(cross(subtract(q[1].attribute.position,q[0].attribute.position),subtract(q[2].attribute.position,q[0].attribute.position)));
                if(std::abs(dot(n,subtract(q[3].attribute.position,q[0].attribute.position)))>tolerance)return fail("Open bevel requires planar quads");
                // Check affine UV/color domains before replacing a face's hidden triangulation.
                const auto x=subtract(q[1].attribute.position,q[0].attribute.position),y=subtract(q[2].attribute.position,q[0].attribute.position),p=subtract(q[3].attribute.position,q[0].attribute.position);const auto xx=dot(x,x),xy=dot(x,y),yy=dot(y,y),den=xx*yy-xy*xy;
                if(den<=extent*extent*1e-12F)return fail("Open face domain is degenerate");const auto u=(dot(p,x)*yy-dot(p,y)*xy)/den,v=(dot(p,y)*xx-dot(p,x)*xy)/den;
                const auto test=[&](float a,float b,float c,float actual){return std::abs(a+u*(b-a)+v*(c-a)-actual)<=1e-5F*(std::max)({1.F,std::abs(a),std::abs(b),std::abs(c),std::abs(actual)});};
                const auto& a=q[0].attribute;const auto& b=q[1].attribute;const auto& c=q[2].attribute;const auto& d=q[3].attribute;
                if(!test(a.texcoord.x,b.texcoord.x,c.texcoord.x,d.texcoord.x)||!test(a.texcoord.y,b.texcoord.y,c.texcoord.y,d.texcoord.y)||!test(a.texcoord1.x,b.texcoord1.x,c.texcoord1.x,d.texcoord1.x)||!test(a.texcoord1.y,b.texcoord1.y,c.texcoord1.y,d.texcoord1.y)||!test(a.color.x,b.color.x,c.color.x,d.color.x)||!test(a.color.y,b.color.y,c.color.y,d.color.y)||!test(a.color.z,b.color.z,c.color.z,d.color.z)||!test(a.color.w,b.color.w,c.color.w,d.color.w))return fail("Resolve non-affine UV/color quad domains before local open bevel");
            }
            // A face-side component is determined by adjacency, never by matching moved positions.
            std::map<Index,Index> side;
            std::map<Index,std::set<Index>> adjacency;for(const auto& [e,h]:halves)if(!selection.components.contains(e)&&h.size()==2&&h[0].face!=h[1].face&&dot(h[0].normal,h[1].normal)>.99999F){adjacency[h[0].face].insert(h[1].face);adjacency[h[1].face].insert(h[0].face);}
            for(auto seed:touched)if(!side.contains(seed)){side[seed]=seed;std::queue<Index> pending;pending.push(seed);while(!pending.empty()){auto f=pending.front();pending.pop();for(auto next:adjacency[f])if(touched.contains(next)&&!side.contains(next)){side[next]=seed;pending.push(next);}}}
            std::set<Index> sides;for(auto [f,s]:side)sides.insert(s);if(sides.size()!=(boundary?1U:2U))return fail("Open chain does not have consistent planar face sides");
            std::map<std::pair<Index,Index>,Vertex> rails;std::map<Index,Patch> trimmed;
            for(auto f:touched){const auto e=faceEdge.at(f);const auto& h=*std::find_if(halves.at(e).begin(),halves.at(e).end(),[&](const auto& h){return h.face==f;});const auto direction=normalize(subtract(positions.at(h.b),positions.at(h.a)));const auto inward=normalize(cross(h.normal,direction));const auto sine=boundary?1.F:std::sqrt((1-dot(halves.at(e)[0].normal,halves.at(e)[1].normal))*.5F);const auto setback=width/sine;
                auto q=quads.at(f);const auto originalQuad=q;for(std::size_t j=0;j<4;++j)if(originalQuad[j].group==h.a||originalQuad[j].group==h.b){const auto k=originalQuad[(j+1)%4].group==h.a||originalQuad[(j+1)%4].group==h.b?(j+3)%4:(j+1)%4;const auto delta=subtract(originalQuad[k].attribute.position,originalQuad[j].attribute.position);const auto length=dot(delta,inward);if(length<=setback+tolerance||std::abs(dot(normalize(delta),direction))>1e-5F||dot(normalize(delta),inward)<.99999F)return fail("Open bevel needs perpendicular end rails and room for its setback");
                    const auto key=std::pair{originalQuad[j].group,side.at(f)};auto found=rails.find(key);Vertex rail;
                    if(found==rails.end()){const auto point=add(originalQuad[j].attribute.position,multiply(inward,setback));rail=interpolate(originalQuad[j],originalQuad[k],setback/length,nextGroup++,point);rail.geometricParent=originalQuad[j].group;positions[rail.group]=point;tags[rail.group]={edge(originalQuad[j].group,originalQuad[k].group)};rails.emplace(key,rail);}else{rail=interpolate(originalQuad[j],originalQuad[k],setback/length,found->second.group,found->second.attribute.position);rail.geometricParent=originalQuad[j].group;}
                    q[j]=std::move(rail);}
                Patch patch;patch.vertices=q;patch.face=f;patch.material=materials[faces[f][0]];trimmed[f]=std::move(patch);}
            std::vector<Patch> local;for(auto patch:patches)if(!touched.contains(*patch.face))local.push_back(std::move(patch));for(auto& [f,p]:trimmed)local.push_back(std::move(p));
            std::map<std::pair<Index,Index>,Index> profileGroups;
            for(auto e:selection.components){auto h=halves.at(e);std::sort(h.begin(),h.end(),[&](const auto& a,const auto& b){return side.at(a.face)<side.at(b.face);});const auto a=h[0].a,b=h[0].b;const auto d0=normalize(cross(h[0].normal,normalize(subtract(positions.at(b),positions.at(a)))));
                const auto d1=boundary?multiply(h[0].normal,-1):normalize(cross(h[1].normal,normalize(subtract(positions.at(h[1].b),positions.at(h[1].a)))));
                const auto sine=boundary?1.F:std::sqrt((1-dot(h[0].normal,h[1].normal))*.5F);const auto profile=profile_points(d0,d1,width/sine,settings);const auto count=Index(profile.size()-1);
                std::array<std::vector<Vertex>,2> ends;
                for(Index endpoint=0;endpoint<2;++endpoint){const auto old=endpoint?b:a;const auto r0=rails.at({old,side.at(h[0].face)});Vertex r1;
                    if(!boundary)r1=rails.at({old,side.at(h[1].face)});else{r1=r0;r1.original.reset();r1.corner.reset();const auto attribute=endpoint?h[0].vb:h[0].va;r1.attribute=input.vertices[attribute];r1.attributeParents={attribute};r1.attribute.position=add(positions.at(old),profile.back());r1.geometricParent=old;}
                    for(Index step=0;step<=count;++step){auto v=interpolate(r0,r1,float(step)/float(count),0,add(positions.at(old),profile[step]));v.geometricParent=old;
                        if(step==0)v.group=r0.group;else if(step==count&&!boundary)v.group=r1.group;else{const auto key=std::pair{old,step};auto [it,created]=profileGroups.emplace(key,nextGroup);if(created)++nextGroup;v.group=it->second;}
                        positions[v.group]=v.attribute.position;if(boundary&&step==count)tags[v.group].insert(e);ends[endpoint].push_back(std::move(v));}}
                for(Index step=0;step<count;++step){Patch strip;strip.bevelEdge=e;strip.material=selectedMaterial.at(e);strip.vertices={ends[1][step],ends[0][step],ends[0][step+1],ends[1][step+1]};local.push_back(std::move(strip));}
            }
            patches=std::move(local);
        }
        // Emit explicit triangle/quad records. Unmodified saved quads retain their grouping.
        auto& mesh=result.polygon.mesh;mesh.vertices.clear();mesh.indices.clear();mesh.submeshes.clear();auto& lineage=result.lineage;
        for(Index t=0;t<materials.size();++t)lineage.triangleDescendants[t];for(Index c=0;c<source.indices.size();++c)lineage.cornerDescendants[c];for(const auto& [e,h]:halves)lineage.edgeDescendants[e];
        SubdivisionSettings updated=authored?*authored:SubdivisionSettings{};updated.controlFaces.clear();updated.edgeCreases.clear();std::map<Index,Index> survivors;
        struct Record {std::vector<Index> vertices;std::optional<Index> face;bool unchanged{};};std::vector<Record> records;std::vector<std::optional<Edge>> triangleBevel;std::vector<std::optional<Index>> triangleFace;
        std::vector<bool> generated;
        result.polygon.selection={selection.object,PolygonSelectionMode::Face,{}};
        const auto appendVertex=[&](const Vertex& v,bool bevel){if(v.original){if(auto at=survivors.find(*v.original);at!=survivors.end())return at->second;}
            if(mesh.vertices.size()>=100000)throw std::runtime_error("Bevel output exceeds its attribute budget");
            const auto i=Index(mesh.vertices.size());auto attribute=v.attribute;if(world)attribute.position=v.group<source.vertices.size()?source.vertices[v.group].position:Float3{attribute.position.x/objectScale.x,attribute.position.y/objectScale.y,attribute.position.z/objectScale.z};mesh.vertices.push_back(attribute);generated.push_back(bevel);lineage.afterVertexGroups.push_back(v.group);
            if(v.original){survivors[*v.original]=i;lineage.vertexSurvivors[*v.original]=i;}else for(auto parent:v.attributeParents)lineage.attributeCopies[parent].insert(i);
            if(v.geometricParent)lineage.geometricVertexCopies[*v.geometricParent].insert(i);return i;};
        for(const auto& patch:patches){std::vector<Index> indices;for(auto v:patch.vertices){if(patch.bevelEdge){v.original.reset();v.corner.reset();}indices.push_back(appendVertex(v,patch.bevelEdge.has_value()));}
            const auto first=Index(mesh.indices.size()/3);std::vector<Index> rendered;
            const auto normal=patch_normal(patch);
            // Rail subdivisions can differ from exact collinearity by a few float ULPs.
            // Keep those vertices in larger ears instead of leaving a collapsing sliver.
            float patchExtent=0;for(const auto& v:patch.vertices)patchExtent=(std::max)(patchExtent,length_squared(subtract(v.attribute.position,patch.vertices[0].attribute.position)));
            const auto areaTolerance=patchExtent*1e-7F;std::vector<std::array<Index,3>> triangulation;std::vector<Index> ring;for(Index j=0;j<indices.size();++j)ring.push_back(j);
            while(ring.size()>3){bool found=false;for(std::size_t j=0;j<ring.size();++j){const auto a=ring[(j+ring.size()-1)%ring.size()],b=ring[j],c=ring[(j+1)%ring.size()];const auto pa=patch.vertices[a].attribute.position,pb=patch.vertices[b].attribute.position,pc=patch.vertices[c].attribute.position;
                const auto orient=[&](Float3 x,Float3 y,Float3 z){return dot(cross(subtract(y,x),subtract(z,x)),normal);};if(orient(pa,pb,pc)<=areaTolerance)continue;bool inside=false;
                for(auto other:ring)if(other!=a&&other!=b&&other!=c){const auto p=patch.vertices[other].attribute.position;if(orient(pa,pb,p)>=-areaTolerance&&orient(pb,pc,p)>=-areaTolerance&&orient(pc,pa,p)>=-areaTolerance){inside=true;break;}}
                if(inside)continue;triangulation.push_back({a,b,c});ring.erase(ring.begin()+std::ptrdiff_t(j));found=true;break;}if(!found)return fail("Bevel patch cannot be triangulated without degenerate faces");}
            if(dot(cross(subtract(patch.vertices[ring[1]].attribute.position,patch.vertices[ring[0]].attribute.position),subtract(patch.vertices[ring[2]].attribute.position,patch.vertices[ring[0]].attribute.position)),normal)<=areaTolerance)return fail("Bevel patch leaves a numerically degenerate final triangle");
            triangulation.push_back({ring[0],ring[1],ring[2]});
            bool quad=indices.size()==4;for(Index j=0;quad&&j<4;++j)quad=dot(cross(subtract(patch.vertices[(j+1)%4].attribute.position,patch.vertices[j].attribute.position),subtract(patch.vertices[(j+2)%4].attribute.position,patch.vertices[(j+1)%4].attribute.position)),normal)>areaTolerance;
            for(const auto& corners:triangulation){if(mesh.indices.size()+3>150000)throw std::runtime_error("Bevel output exceeds its triangle budget");const auto triangle=Index(mesh.indices.size()/3);for(auto corner:corners){const auto outputCorner=Index(mesh.indices.size());mesh.indices.push_back(indices[corner]);rendered.push_back(indices[corner]);if(patch.vertices[corner].corner&&!patch.bevelEdge)lineage.cornerDescendants[*patch.vertices[corner].corner].insert(outputCorner);if(patch.face&&!patch.triangle&&!patch.bevelEdge&&patch.vertices[corner].original)for(auto old:faces[*patch.face])for(Index j=0;j<3;++j)if(source.indices[old*3+j]==*patch.vertices[corner].original)lineage.cornerDescendants[old*3+j].insert(outputCorner);}
                if(patch.triangle)lineage.triangleDescendants[*patch.triangle].insert(triangle);else if(patch.face)for(auto old:faces[*patch.face])lineage.triangleDescendants[old].insert(triangle);
                if(patch.bevelEdge)result.polygon.selection.components.insert(triangle);triangleBevel.push_back(patch.bevelEdge);triangleFace.push_back(patch.face);
                if(mesh.submeshes.empty()||mesh.submeshes.back().materialIndex!=patch.material)mesh.submeshes.push_back({"Authored bevel",triangle*3,3,patch.material});else mesh.submeshes.back().indexCount+=3;}
            bool unchanged=patch.triangle&&patch.vertices.size()==3;for(const auto& v:patch.vertices)unchanged=unchanged&&v.original.has_value();
            if(indices.size()==3||quad)records.push_back({rendered,patch.face,unchanged});else for(Index t=first;t<mesh.indices.size()/3;++t)records.push_back({{mesh.indices[t*3],mesh.indices[t*3+1],mesh.indices[t*3+2]},patch.face,false});
        }
        std::map<Index,std::vector<Index>> faceRecords;for(Index r=0;r<records.size();++r)if(records[r].face)faceRecords[*records[r].face].push_back(r);
        std::set<Index> emitted;for(Index f=0;f<faces.size();++f){const auto& children=faceRecords[f];
            if(children.size()==2&&faces[f].size()==2&&records[children[0]].unchanged&&records[children[1]].unchanged){auto record=records[children[0]].vertices;record.insert(record.end(),records[children[1]].vertices.begin(),records[children[1]].vertices.end());lineage.controlFaceDescendants[f]={Index(updated.controlFaces.size())};updated.controlFaces.push_back(std::move(record));emitted.insert(children.begin(),children.end());}
            else for(auto r:children){lineage.controlFaceDescendants[f].push_back(Index(updated.controlFaces.size()));updated.controlFaces.push_back(records[r].vertices);emitted.insert(r);}}
        for(Index r=0;r<records.size();++r)if(!emitted.contains(r))updated.controlFaces.push_back(records[r].vertices);
        if(!authored){lineage.controlFaceDescendants.clear();for(const auto& [t,next]:lineage.triangleDescendants)lineage.controlFaceDescendants[t]={next.begin(),next.end()};}
        std::map<Index,Index> representative;for(Index i=0;i<mesh.vertices.size();++i)representative.try_emplace(lineage.afterVertexGroups[i],i);
        std::map<Edge,std::vector<Index>> outputEdges;
        for(Index t=0;t<mesh.indices.size()/3;++t)for(Index j=0;j<3;++j){const auto a=lineage.afterVertexGroups[mesh.indices[t*3+j]],b=lineage.afterVertexGroups[mesh.indices[t*3+(j+1)%3]];outputEdges[edge(a,b)].push_back(t);}
        for(const auto& [e,triangles]:outputEdges){const auto a=Index(e>>32U),b=Index(e);const auto descendant=edge(representative.at(a),representative.at(b));std::set<Edge> parents;
            std::set_intersection(tags[a].begin(),tags[a].end(),tags[b].begin(),tags[b].end(),std::inserter(parents,parents.end()));for(auto parent:parents)lineage.edgeDescendants[parent].insert(descendant);
            if(triangles.size()==2){const auto x=triangleBevel[triangles[0]],y=triangleBevel[triangles[1]];if(x&&!y)lineage.edgeDescendants[*x].insert(descendant);if(y&&!x)lineage.edgeDescendants[*y].insert(descendant);}}
        refresh_editor_polygon(mesh);
        // Keep surviving source shading exactly; smooth only new profile attributes.
        for(const auto& [old,next]:lineage.vertexSurvivors)mesh.vertices[next]=source.vertices[old];
        const auto cosine=std::cos(settings.smoothingAngle*3.14159265358979323846F/180);std::map<Index,std::vector<Float3>> normals;std::vector<Float3> flat(mesh.vertices.size());
        for(Index t=0;t<mesh.indices.size()/3;++t){const auto a=mesh.vertices[mesh.indices[t*3]].position,b=mesh.vertices[mesh.indices[t*3+1]].position,c=mesh.vertices[mesh.indices[t*3+2]].position;const auto n=normalize(cross(subtract(b,a),subtract(c,a)));for(Index j=0;j<3;++j){const auto v=mesh.indices[t*3+j];normals[lineage.afterVertexGroups[v]].push_back(n);if(generated[v])flat[v]=n;}}
        for(Index i=0;i<mesh.vertices.size();++i)if(generated[i]){Float3 n{};for(const auto normal:normals[lineage.afterVertexGroups[i]])if(dot(flat[i],normal)>=cosine-1e-6F)n=add(n,normal);mesh.vertices[i].normal=length_squared(n)>1e-12F?normalize(n):flat[i];}
        refresh_editor_polygon(mesh);for(const auto& [old,next]:lineage.vertexSurvivors)mesh.vertices[next]=source.vertices[old];mesh.contentHash=polygon_asset_content_hash(mesh);
        if(mesh.vertices.size()>100000||mesh.indices.size()>150000)return fail("Bevel output exceeds its modeling budget");
        if(const auto valid=validate_polygon_asset(mesh);!valid)return fail(valid.message);
        const auto actual=polygon_geometric_vertices(mesh);std::map<Index,Index> forward,back;
        for(Index i=0;i<actual.size();++i){const auto group=lineage.afterVertexGroups[i];if(auto [at,added]=forward.emplace(group,actual[i]);!added&&at->second!=actual[i])return fail("Bevel split one geometric group inconsistently");if(auto [at,added]=back.emplace(actual[i],group);!added&&at->second!=group)return fail("Bevel would collapse independent vertices; reduce width or segments");}
        const auto after=diagnose_polygon_topology(mesh);if(after.nonmanifold||after.inconsistentWinding||after.degenerate||(convex&&after.boundaries))return fail("Bevel produces invalid topology or unsupported corner joins");
        auto check=updated;check.level=0;const auto selectable=subdivision_control_edges(mesh,&updated);
        if(authored)for(const auto& [old,strength]:authored->edgeCreases){const auto& next=lineage.edgeDescendants.at(old);if(next.empty())return fail("Bevel retires an authored crease without supported descendants");for(auto e:next){if(!selectable.contains(e))return fail("Bevel crease maps onto a hidden diagonal");if(auto at=updated.edgeCreases.find(e);at!=updated.edgeCreases.end()&&at->second!=strength)return fail("Bevel combines conflicting crease strengths");updated.edgeCreases[e]=strength;}}
        check.edgeCreases=updated.edgeCreases;if(const auto evaluated=evaluate_subdivision(mesh,check);!evaluated.mesh)return fail("Control cage: "+evaluated.error);
        if(authored&&updated.level!=0)if(const auto evaluated=evaluate_subdivision(mesh,updated);!evaluated.mesh)return fail("Evaluated bevel: "+evaluated.error);
        if(!make_modeling_topology(mesh,authored?&updated:nullptr,lineage.afterVertexGroups,&error))return fail(error);
        if(result.polygon.selection.components.empty())return fail("Bevel has no generated profile faces");
        if(authored)result.subdivision=std::move(updated);return result;
    }catch(const std::exception& e){return fail(e.what());}
}
} // namespace dve::editor
