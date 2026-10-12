#include "dve/editor_mesh_authoring.hpp"
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
    }else{
        if(selection.mode!=PolygonSelectionMode::Edge)return fail("Select boundary edges for bridge/cap");
        std::map<std::uint32_t,Half> next;for(auto k:selection.components){const auto& h=t.edges.at(k);if(h.size()!=1)return fail("Only open boundary edges can be bridged/capped");if(!next.emplace(h[0].a,h[0]).second)return fail("Boundary branches at a vertex");}
        std::vector<std::vector<Half>> loops;while(!next.empty()){std::vector<Half> loop;auto start=next.begin()->first,current=start;do{auto it=next.find(current);if(it==next.end())return fail("Select complete closed boundary loops");auto h=it->second;next.erase(it);loop.push_back(h);current=h.b;}while(current!=start&&loop.size()<=4096);if(current!=start||loop.size()<3)return fail("Invalid or oversized boundary loop");loops.push_back(std::move(loop));}
        outMat=mat;r.selection.mode=PolygonSelectionMode::Face;r.selection.components.clear();auto first=outMat.size();
        if(operation==TopologyEdit::BridgeLoops){
            if(loops.size()!=2||loops[0].size()!=loops[1].size())return fail("Bridge needs two boundary loops with equal vertex counts");
            auto& A=loops[0];auto& B=loops[1];const auto n=A.size();std::size_t shift=0;double best=INFINITY;
            for(std::size_t k=0;k<n;++k){double cost=0;for(std::size_t j=0;j<n;++j)cost+=length_squared(subtract(source.vertices[A[j].va].position,source.vertices[B[(k+n-j)%n].va].position));if(cost<best){best=cost;shift=k;}}
            for(std::size_t j=0;j<n;++j){auto a=A[j].va,b=A[(j+1)%n].va,c=B[(shift+n-j)%n].va,d=B[(shift+n-j-1)%n].va;append(b,a,c,mat[A[j].face]);append(b,c,d,mat[A[j].face]);}
        }else{
            if(loops.size()!=1)return fail("Cap one simple contour at a time; nested holes need a separate triangulation tool");
            std::vector<std::uint32_t> loop;for(const auto& h:loops[0])loop.push_back(h.va);std::reverse(loop.begin(),loop.end());
            Float3 normal{};for(std::size_t j=0;j<loop.size();++j)normal=add(normal,cross(source.vertices[loop[j]].position,source.vertices[loop[(j+1)%loop.size()]].position));normal=normalize(normal);
            if(length_squared(normal)<.5F)return fail("Cap contour has no stable plane");auto origin=source.vertices[loop[0]].position;
            for(auto v:loop)if(std::abs(dot(normal,subtract(source.vertices[v].position,origin)))>1e-5F)return fail("Cap contour must be planar");
            auto u=normalize(cross(normal,std::abs(normal.y)<.9F?Float3{0,1,0}:Float3{1,0,0})),v=cross(normal,u);
            auto point=[&](auto i){auto p=subtract(source.vertices[i].position,origin);return Float2{dot(p,u),dot(p,v)};};
            auto orient=[](Float2 a,Float2 b,Float2 c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);};
            while(loop.size()>2){bool found=false;for(std::size_t j=0;j<loop.size();++j){auto a=loop[(j+loop.size()-1)%loop.size()],b=loop[j],c=loop[(j+1)%loop.size()];auto pa=point(a),pb=point(b),pc=point(c);if(orient(pa,pb,pc)<=1e-12F)continue;bool inside=false;
                    for(auto other:loop){if(other==a||other==b||other==c)continue;auto p=point(other);if(orient(pa,pb,p)>=-1e-12F&&orient(pb,pc,p)>=-1e-12F&&orient(pc,pa,p)>=-1e-12F){inside=true;break;}}
                    if(inside)continue;append(a,b,c,mat[loops[0][0].face]);loop.erase(loop.begin()+std::ptrdiff_t(j));found=true;break;}
                if(!found)return fail("Cap contour is self-intersecting or degenerate");}
        }
        for(auto f=first;f<outMat.size();++f)r.selection.components.insert(f);surfaces(m,outMat);
    }
    if(m.vertices.size()>1000000||m.indices.size()>6000000)return fail("Topology edit exceeds geometry budget");
    refresh_editor_polygon(m);const auto valid=validate_polygon_asset(m);if(!valid)return fail(valid.message);
    const auto d=diagnose_polygon_topology(m),before=diagnose_polygon_topology(source);
    if(d.nonmanifold>before.nonmanifold||d.inconsistentWinding>before.inconsistentWinding||d.degenerate>before.degenerate)return fail("Edit would introduce invalid topology");
    return r;
}
} // namespace dve::editor
