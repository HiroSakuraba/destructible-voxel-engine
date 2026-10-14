#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_modeling_topology.hpp"
#include "dve/editor_subdivision.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
namespace dve::editor {
namespace {
Float3 cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
std::uint64_t key(std::uint32_t a,std::uint32_t b){return (std::uint64_t(std::min(a,b))<<32U)|std::max(a,b);}
struct Half{std::uint32_t a,b,face,va,vb;};
struct Topology{std::vector<std::uint32_t> canonical;std::map<std::uint64_t,std::vector<Half>> edges;};
Topology topology(const CookedPolygonAsset& m){
    Topology t;std::map<std::tuple<float,float,float>,std::uint32_t> points;
    for(std::uint32_t i=0;i<m.vertices.size();++i){auto p=m.vertices[i].position;auto [it,newPoint]=points.emplace(std::tuple{p.x,p.y,p.z},i);(void)newPoint;t.canonical.push_back(it->second);}
    for(std::uint32_t f=0;f<m.indices.size()/3;++f)for(std::uint32_t j=0;j<3;++j){auto va=m.indices[f*3+j],vb=m.indices[f*3+(j+1)%3];auto a=t.canonical[va],b=t.canonical[vb];t.edges[key(a,b)].push_back({a,b,f,va,vb});}
    return t;
}
std::vector<std::uint32_t> materials(const CookedPolygonAsset& m){std::vector<std::uint32_t> out(m.indices.size()/3);for(const auto& s:m.submeshes)for(std::size_t i=s.firstIndex/3;i<(s.firstIndex+s.indexCount)/3;++i)out[i]=s.materialIndex;return out;}
void surfaces(CookedPolygonAsset& m,const std::vector<std::uint32_t>& mat){m.submeshes.clear();for(std::size_t i=0;i<mat.size();++i){if(m.submeshes.empty()||m.submeshes.back().materialIndex!=mat[i])m.submeshes.push_back({"Edited topology",std::uint32_t(i*3),3,mat[i]});else m.submeshes.back().indexCount+=3;}}
PolygonVertex interpolate(PolygonVertex a,const PolygonVertex& b,float t){
    a.position=add(a.position,multiply(subtract(b.position,a.position),t));a.normal=normalize(add(a.normal,multiply(subtract(b.normal,a.normal),t)));
    auto mix=[t](float x,float y){return x+(y-x)*t;};a.texcoord={mix(a.texcoord.x,b.texcoord.x),mix(a.texcoord.y,b.texcoord.y)};a.texcoord1={mix(a.texcoord1.x,b.texcoord1.x),mix(a.texcoord1.y,b.texcoord1.y)};
    a.color={mix(a.color.x,b.color.x),mix(a.color.y,b.color.y),mix(a.color.z,b.color.z),mix(a.color.w,b.color.w)};return a;
}
}
TopologyDiagnostics diagnose_polygon_topology(const CookedPolygonAsset& mesh){
    TopologyDiagnostics d;if(!validate_polygon_asset(mesh)){d.degenerate=1;return d;}const auto t=topology(mesh);
    for(const auto& [k,halves]:t.edges){if(halves.size()==1)++d.boundaries;else if(halves.size()!=2)++d.nonmanifold;
        else if(halves[0].a==halves[1].a)++d.inconsistentWinding;
        if(halves.size()!=2||(halves.size()==2&&halves[0].a==halves[1].a))d.problemEdges.insert(k);}
    for(std::size_t i=0;i<mesh.indices.size();i+=3){const auto a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;
        if(length_squared(cross(subtract(b,a),subtract(c,a)))<=1e-20F)++d.degenerate;}
    return d;
}
PolygonEditResult edit_polygon_topology(const CookedPolygonAsset& source,const PolygonSelection& selection,TopologyEdit operation,float amount){
    PolygonEditResult r{source,selection,{}};auto fail=[&](std::string s){r.mesh=source;r.selection=selection;r.error=std::move(s);return r;};
    if(!validate_polygon_asset(source)||!polygon_selection_valid(source,selection)||selection.components.empty())return fail("Select valid mesh components first");
    if(!std::isfinite(amount))return fail("Topology parameter must be finite");
    if(operation==TopologyEdit::BevelConvexEdges)return bevel_convex_editor_edges(source,selection,amount);
    if(operation==TopologyEdit::BridgeLoops||operation==TopologyEdit::CapBoundaries)return edit_polygon_boundaries(source,selection,operation).polygon;
    auto& m=r.mesh;const auto t=topology(source);const auto mat=materials(source);std::vector<std::uint32_t> outMat;
    auto append=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c,std::uint32_t material){m.indices.insert(m.indices.end(),{a,b,c});outMat.push_back(material);};
    const auto selectedFaces=polygon_selected_faces(source,selection);
    if(operation==TopologyEdit::Subdivide||operation==TopologyEdit::SplitEdges){
        if(!(amount>0&&amount<1))return fail("Split position must lie strictly inside the edge");
        std::set<std::uint64_t> split;
        if(operation==TopologyEdit::SplitEdges){if(selection.mode!=PolygonSelectionMode::Edge)return fail("Select edges for Split Edges");split=selection.components;}
        else for(auto f:selectedFaces)for(std::uint32_t j=0;j<3;++j)split.insert(key(t.canonical[source.indices[f*3+j]],t.canonical[source.indices[f*3+(j+1)%3]]));
        std::map<std::uint64_t,std::uint32_t> midpoints;m.indices.clear();r.selection.mode=PolygonSelectionMode::Face;r.selection.components.clear();
        for(std::uint32_t f=0;f<source.indices.size()/3;++f){std::array<std::uint32_t,3> v{},mid{};std::array<bool,3> cut{};unsigned n=0;
            for(std::uint32_t j=0;j<3;++j){v[j]=source.indices[f*3+j];auto b=source.indices[f*3+(j+1)%3];auto k=key(t.canonical[v[j]],t.canonical[b]);cut[j]=split.contains(k);if(!cut[j])continue;++n;
                auto actual=key(v[j],b);auto it=midpoints.find(actual);if(it==midpoints.end()){
                    float fraction=t.canonical[v[j]]<t.canonical[b]?amount:1-amount;auto index=std::uint32_t(m.vertices.size());auto vertex=interpolate(source.vertices[v[j]],source.vertices[b],fraction);
                    auto lo=std::min(t.canonical[v[j]],t.canonical[b]),hi=std::max(t.canonical[v[j]],t.canonical[b]);
                    vertex.position=add(source.vertices[lo].position,multiply(subtract(source.vertices[hi].position,source.vertices[lo].position),amount));
                    m.vertices.push_back(vertex);it=midpoints.emplace(actual,index).first;}mid[j]=it->second;}
            auto first=outMat.size();
            if(n==0)append(v[0],v[1],v[2],mat[f]);
            else if(n==1){unsigned j=0;while(!cut[j])++j;append(v[j],mid[j],v[(j+2)%3],mat[f]);append(mid[j],v[(j+1)%3],v[(j+2)%3],mat[f]);}
            else if(n==2){unsigned j=0;while(cut[j])++j;auto a=(j+1)%3,b=(j+2)%3;append(v[b],mid[b],mid[a],mat[f]);append(v[j],v[a],mid[a],mat[f]);append(v[j],mid[a],mid[b],mat[f]);}
            else {for(unsigned j=0;j<3;++j)append(v[j],mid[j],mid[(j+2)%3],mat[f]);append(mid[0],mid[1],mid[2],mat[f]);}
            if(n||selectedFaces.contains(f))for(auto child=first;child<outMat.size();++child)r.selection.components.insert(child);
        }
        surfaces(m,outMat);
    }else if(operation==TopologyEdit::FlipDiagonal){
        if(selection.mode!=PolygonSelectionMode::Edge||selection.components.size()!=1)return fail("Select one internal triangle diagonal");
        const auto& h=t.edges.at(*selection.components.begin());if(h.size()!=2||h[0].a!=h[1].b)return fail("Diagonal must have two oppositely wound adjacent triangles");
        if(mat[h[0].face]!=mat[h[1].face])return fail("Cannot flip across a material boundary");
        // Attribute seams are intentional boundaries; flipping through one would lose UV/normal data.
        if(h[0].va!=h[1].vb||h[0].vb!=h[1].va)return fail("Cannot flip across a UV or normal seam");
        auto opposite=[&](const Half& e){for(unsigned j=0;j<3;++j){auto v=source.indices[e.face*3+j];if(v!=e.va&&v!=e.vb)return v;}return e.va;};
        auto a=h[0].va,b=h[0].vb,c=opposite(h[0]),d=opposite(h[1]);
        auto p=[&](auto i){return source.vertices[i].position;};auto normal=normalize(cross(subtract(p(b),p(a)),subtract(p(c),p(a))));
        if(std::abs(dot(normal,subtract(p(d),p(a))))>1e-5F||dot(cross(subtract(p(d),p(c)),subtract(p(b),p(c))),normal)<=0||
            dot(cross(subtract(p(c),p(d)),subtract(p(a),p(d))),normal)<=0)return fail("Diagonal flip needs a convex coplanar quad");
        const std::array<std::uint32_t,3> one{c,d,b},two{d,c,a};for(unsigned j=0;j<3;++j){m.indices[h[0].face*3+j]=one[j];m.indices[h[1].face*3+j]=two[j];}
        r.selection.mode=PolygonSelectionMode::Face;r.selection.components={h[0].face,h[1].face};
    }else if(operation==TopologyEdit::HardNormals||operation==TopologyEdit::SmoothNormals){
        if(operation==TopologyEdit::HardNormals){for(auto f:selectedFaces){auto a=source.vertices[source.indices[f*3]].position,b=source.vertices[source.indices[f*3+1]].position,c=source.vertices[source.indices[f*3+2]].position;
                auto n=normalize(cross(subtract(b,a),subtract(c,a)));for(unsigned j=0;j<3;++j){auto v=source.vertices[source.indices[f*3+j]];v.normal=n;m.indices[f*3+j]=std::uint32_t(m.vertices.size());m.vertices.push_back(v);}}}
        else {std::map<std::uint32_t,Float3> normals;for(auto f:selectedFaces){auto a=source.vertices[source.indices[f*3]].position,b=source.vertices[source.indices[f*3+1]].position,c=source.vertices[source.indices[f*3+2]].position;
                auto n=cross(subtract(b,a),subtract(c,a));for(unsigned j=0;j<3;++j){auto v=t.canonical[source.indices[f*3+j]];normals[v]=add(normals[v],n);}}
            // Split selected corner attributes first, so smoothing never changes unselected faces.
            for(auto f:selectedFaces)for(unsigned j=0;j<3;++j){auto index=source.indices[f*3+j];auto v=source.vertices[index];v.normal=normalize(normals[t.canonical[index]]);m.indices[f*3+j]=std::uint32_t(m.vertices.size());m.vertices.push_back(v);}}
    }else return fail("Unsupported topology edit");
    if(m.vertices.size()>1000000||m.indices.size()>6000000)return fail("Topology edit exceeds geometry budget");
    refresh_editor_polygon(m);const auto valid=validate_polygon_asset(m);if(!valid)return fail(valid.message);
    const auto d=diagnose_polygon_topology(m),before=diagnose_polygon_topology(source);
    if(d.nonmanifold>before.nonmanifold||d.inconsistentWinding>before.inconsistentWinding||d.degenerate>before.degenerate)return fail("Edit would introduce invalid topology");
    return r;
}
PolygonFaceEditResult edit_polygon_boundaries(const CookedPolygonAsset& source,const PolygonSelection& selection,
    TopologyEdit operation,const SubdivisionSettings* authored) {
    PolygonFaceEditResult result;result.polygon={source,selection,{}};
    const auto fail=[&](std::string message){result.polygon={source,selection,std::move(message)};result.lineage={};result.subdivision.reset();return result;};
    if(operation!=TopologyEdit::BridgeLoops&&operation!=TopologyEdit::CapBoundaries)return fail("Choose Bridge or Cap");
    if(source.vertices.size()>100000||source.indices.size()>150000)return fail("Boundary tool exceeds its modeling budget");
    if(!validate_polygon_asset(source)||!polygon_selection_valid(source,selection)||selection.mode!=PolygonSelectionMode::Edge||selection.components.empty())
        return fail("Select complete open boundary loops");
    if(selection.components.size()>1024)return fail("Boundary tools support at most 512 edges per loop");
    const auto before=diagnose_polygon_topology(source);
    if(before.nonmanifold||before.inconsistentWinding||before.degenerate)return fail("Boundary tools require consistently wound manifold input");
    std::string error;
    // Reject ambiguous legacy position partitions and disconnected vertex fans, including bow-ties.
    if(!make_modeling_topology(source,authored,{},&error))return fail(error);
    const auto t=topology(source);const auto mat=materials(source);
    // Joined meshes can contain distinct palette slots with identical definitions and bindings.
    std::map<std::uint32_t,std::uint64_t> materialHashes;
    const auto material_hash=[&](std::uint32_t index) {
        if(const auto at=materialHashes.find(index);at!=materialHashes.end())return at->second;
        CookedPolygonAsset material;material.materials={source.materials[index]};material.materialBindings={source.materialBindings[index]};
        const auto hash=polygon_asset_content_hash(material);materialHashes[index]=hash;return hash;
    };
    const auto same_material=[&](std::uint32_t a,std::uint32_t b){return a==b||material_hash(a)==material_hash(b);};
    std::map<std::uint32_t,Half> next;std::set<std::uint32_t> incoming;
    for(auto edge:selection.components) {
        const auto at=t.edges.find(edge);
        if(at==t.edges.end()||at->second.size()!=1)return fail("Only open boundary edges can be bridged/capped");
        const auto h=at->second.front();
        if(!next.emplace(h.a,h).second||!incoming.insert(h.b).second)return fail("Boundary branches at a vertex");
    }
    std::vector<std::vector<Half>> loops;
    while(!next.empty()) {
        std::vector<Half> loop;const auto start=next.begin()->first;auto current=start;
        do {
            const auto at=next.find(current);if(at==next.end())return fail("Select complete closed boundary loops");
            const auto h=at->second;next.erase(at);loop.push_back(h);current=h.b;
            if(loop.size()>512)return fail("Boundary tools support at most 512 edges per loop");
        }while(current!=start);
        if(loop.size()<3)return fail("A boundary loop needs at least three vertices");
        loops.push_back(std::move(loop));
    }
    if(operation==TopologyEdit::BridgeLoops&&(loops.size()!=2||loops[0].size()!=loops[1].size()))
        return fail("Bridge needs two complete boundary loops with equal vertex counts");
    if(operation==TopologyEdit::CapBoundaries&&loops.size()!=1)return fail("Cap one simple contour at a time; nested holes are unsupported");
    const auto count=loops.front().size();
    const auto addedVertices=operation==TopologyEdit::BridgeLoops?4*count:3*(count-2);
    const auto addedIndices=operation==TopologyEdit::BridgeLoops?6*count:3*(count-2);
    if(source.vertices.size()+addedVertices>100000||source.indices.size()+addedIndices>150000)return fail("Boundary tool exceeds its modeling budget");
    auto& mesh=result.polygon.mesh;auto& lineage=result.lineage;lineage=identity_polygon_lineage(source,authored);
    auto outMat=mat;SubdivisionSettings updated=authored?*authored:SubdivisionSettings{};updated.controlFaces.clear();
    if(authored)for(const auto& group:subdivision_control_face_triangles(source,authored)) {
        std::vector<std::uint32_t> record;for(auto triangle:group)for(unsigned j=0;j<3;++j)record.push_back(source.indices[triangle*3+j]);
        updated.controlFaces.push_back(std::move(record));
    }
    else for(std::size_t i=0;i<source.indices.size();i+=3)updated.controlFaces.push_back({source.indices[i],source.indices[i+1],source.indices[i+2]});
    const auto copy=[&](std::uint32_t old,Float3 normal) {
        const auto index=std::uint32_t(mesh.vertices.size());auto vertex=source.vertices[old];vertex.normal=normal;mesh.vertices.push_back(vertex);
        lineage.afterVertexGroups.push_back(t.canonical[old]);lineage.attributeCopies[old].insert(index);return index;
    };
    const auto append=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c,std::uint32_t material) {
        result.polygon.selection.components.insert(outMat.size());mesh.indices.insert(mesh.indices.end(),{a,b,c});outMat.push_back(material);
    };
    const auto normal=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c) {
        return normalize(cross(subtract(source.vertices[b].position,source.vertices[a].position),subtract(source.vertices[c].position,source.vertices[a].position)));
    };
    result.polygon.selection.mode=PolygonSelectionMode::Face;result.polygon.selection.components.clear();
    if(operation==TopologyEdit::BridgeLoops) {
        const auto& A=loops[0];const auto& B=loops[1];std::size_t shift=0;double best=INFINITY;
        // Deterministic reversed correspondence gives opposing half-edge winding at both rims.
        for(std::size_t k=0;k<count;++k) {
            double cost=0;for(std::size_t j=0;j<count;++j)cost+=length_squared(subtract(source.vertices[A[j].va].position,source.vertices[B[(k+count-j)%count].va].position));
            if(cost<best){best=cost;shift=k;}
        }
        for(std::size_t j=0;j<count;++j) {
            const auto& a=A[j];const auto& b=B[(shift+count-j-1)%count];
            if(!same_material(mat[a.face],mat[b.face]))return fail("Bridge cannot choose between conflicting boundary materials");
            const auto n=normal(a.vb,a.va,b.vb);const auto first=mesh.indices.size();
            const std::array<Float3,4> points{source.vertices[a.vb].position,source.vertices[a.va].position,
                source.vertices[b.vb].position,source.vertices[b.va].position};
            float scale=0;for(const auto point:points)scale=(std::max)(scale,length_squared(subtract(point,points[0])));
            if(!std::isfinite(scale)||scale<=0||length_squared(n)<.5F||std::abs(dot(n,subtract(points[3],points[0])))>std::sqrt(scale)*1e-5F)
                return fail("Bridge currently requires convex planar side quads");
            for(unsigned corner=0;corner<4;++corner)
                if(dot(cross(subtract(points[(corner+1)%4],points[corner]),subtract(points[(corner+2)%4],points[(corner+1)%4])),n)<=scale*1e-6F)
                    return fail("Bridge correspondence creates a folded or degenerate side quad");
            const auto p=copy(a.vb,n),q=copy(a.va,n),r=copy(b.vb,n),s=copy(b.va,n);
            append(p,q,r,mat[a.face]);append(p,r,s,mat[a.face]);
            updated.controlFaces.push_back({mesh.indices.begin()+std::ptrdiff_t(first),mesh.indices.end()});
        }
    }else {
        const auto material=mat[loops[0][0].face];
        for(const auto& half:loops[0])if(!same_material(mat[half.face],material))return fail("Cap needs one boundary material; resolve the conflict first");
        std::vector<std::uint32_t> loop;for(const auto& half:loops[0])loop.push_back(half.va);std::reverse(loop.begin(),loop.end());
        const auto origin=source.vertices[loop[0]].position;Float3 area{};float scale=0;
        for(std::size_t j=0;j<count;++j) {
            const auto a=subtract(source.vertices[loop[j]].position,origin),b=subtract(source.vertices[loop[(j+1)%count]].position,origin);
            area=add(area,cross(a,b));scale=(std::max)(scale,length_squared(a));
        }
        if(!std::isfinite(scale)||!std::isfinite(length_squared(area))||scale<=0||length_squared(area)<=scale*scale*1e-12F)return fail("Cap contour has no stable plane");
        const auto n=normalize(area);const auto tolerance=std::sqrt(scale)*1e-5F;
        for(auto index:loop)if(std::abs(dot(n,subtract(source.vertices[index].position,origin)))>tolerance)return fail("Cap contour must be planar");
        const auto u=normalize(cross(n,std::abs(n.y)<.9F?Float3{0,1,0}:Float3{1,0,0})),v=cross(n,u);
        const auto point=[&](auto index){const auto p=subtract(source.vertices[index].position,origin);return Float2{dot(p,u),dot(p,v)};};
        const auto orient=[](Float2 a,Float2 b,Float2 c){return double(b.x-a.x)*(c.y-a.y)-double(b.y-a.y)*(c.x-a.x);};
        const double epsilon=double(scale)*1e-10;
        const auto on_segment=[&](Float2 a,Float2 b,Float2 p){return std::abs(orient(a,b,p))<=epsilon&&double(p.x-a.x)*(p.x-b.x)+double(p.y-a.y)*(p.y-b.y)<=epsilon;};
        for(std::size_t i=0;i<count;++i)for(std::size_t j=i+1;j<count;++j) {
            if(j==i+1||(i==0&&j+1==count))continue;
            const auto a=point(loop[i]),b=point(loop[(i+1)%count]),c=point(loop[j]),d=point(loop[(j+1)%count]);
            const auto ac=orient(a,b,c),ad=orient(a,b,d),ca=orient(c,d,a),cb=orient(c,d,b);
            if(((ac>epsilon&&ad<-epsilon)||(ac<-epsilon&&ad>epsilon))&&((ca>epsilon&&cb<-epsilon)||(ca<-epsilon&&cb>epsilon)))return fail("Cap contour self-intersects");
            if(on_segment(a,b,c)||on_segment(a,b,d)||on_segment(c,d,a)||on_segment(c,d,b))return fail("Cap contour touches or overlaps itself");
        }
        while(loop.size()>2) {
            bool found=false;
            for(std::size_t j=0;j<loop.size();++j) {
                const auto a=loop[(j+loop.size()-1)%loop.size()],b=loop[j],c=loop[(j+1)%loop.size()];const auto pa=point(a),pb=point(b),pc=point(c);
                if(orient(pa,pb,pc)<=epsilon)continue;bool inside=false;
                for(auto other:loop)if(other!=a&&other!=b&&other!=c) {
                    const auto p=point(other);if(orient(pa,pb,p)>=-epsilon&&orient(pb,pc,p)>=-epsilon&&orient(pc,pa,p)>=-epsilon){inside=true;break;}
                }
                if(inside)continue;
                const auto p=copy(a,n),q=copy(b,n),r=copy(c,n);append(p,q,r,material);updated.controlFaces.push_back({p,q,r});
                loop.erase(loop.begin()+std::ptrdiff_t(j));found=true;break;
            }
            if(!found)return fail("Cap contour cannot be triangulated without degenerate faces");
        }
    }
    surfaces(mesh,outMat);refresh_editor_polygon(mesh);
    // Recompute tangents only on new attributes; the existing surface is byte-for-byte intact.
    std::copy(source.vertices.begin(),source.vertices.end(),mesh.vertices.begin());mesh.contentHash=polygon_asset_content_hash(mesh);
    const auto diagnostics=diagnose_polygon_topology(mesh);
    if(diagnostics.nonmanifold||diagnostics.inconsistentWinding||diagnostics.degenerate)return fail("Boundary edit creates invalid topology");
    for(std::size_t i=source.indices.size();i<mesh.indices.size();i+=3) {
        const auto a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;
        const auto scale=(std::max)({length_squared(subtract(b,a)),length_squared(subtract(c,a)),length_squared(subtract(c,b))});
        if(!std::isfinite(scale)||length_squared(cross(subtract(b,a),subtract(c,a)))<=scale*scale*1e-12F)return fail("Boundary edit creates a degenerate face");
    }
    auto check=updated;check.level=0;
    if(const auto evaluated=evaluate_subdivision(mesh,check);!evaluated.mesh)return fail(evaluated.error);
    if(!make_modeling_topology(mesh,authored?&updated:nullptr,lineage.afterVertexGroups,&error))return fail(error);
    if(authored)result.subdivision=std::move(updated);
    return result;
}
} // namespace dve::editor
