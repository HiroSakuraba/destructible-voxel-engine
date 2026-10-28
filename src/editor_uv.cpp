#include "dve/editor_mesh_authoring.hpp"
#include <algorithm>
#include <charconv>
#include <locale>
#include <sstream>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>
namespace dve::editor {

std::vector<std::uint32_t> polygon_geometric_vertices(const CookedPolygonAsset& mesh) {
    std::map<std::tuple<float,float,float>,std::uint32_t> points;
    std::vector<std::uint32_t> result;
    result.reserve(mesh.vertices.size());
    for (std::uint32_t i=0;i<mesh.vertices.size();++i) {
        const auto p=mesh.vertices[i].position;
        const auto [it,inserted]=points.emplace(std::tuple{p.x,p.y,p.z},i);
        (void)inserted;
        result.push_back(it->second);
    }
    return result;
}
std::uint64_t polygon_uv_layout_hash(const CookedPolygonAsset& mesh) {
    const auto geometric=polygon_geometric_vertices(mesh);
    std::uint64_t hash=14695981039346656037ULL;
    const auto mix=[&](std::uint64_t value) {
        for (unsigned i=0;i<8;++i) { hash^=(value>>(i*8))&255U; hash*=1099511628211ULL; }
    };
    mix(mesh.indices.size());
    for (const auto index : mesh.indices) {
        if (index>=geometric.size()) return 0;
        mix(geometric[index]);
    }
    return hash;
}
namespace {
constexpr std::size_t kMaximumUvSeams=40000, kMaximumUvPins=60000, kMaximumUvStateBytes=4*1024*1024;
std::string encode_uv_authoring(const PolygonUvAuthoring& state,std::size_t corners) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out<<"DVE_UV 1 "<<state.layoutHash<<' '<<corners<<'\n';
    for (unsigned channel=0;channel<2;++channel) {
        out<<state.seams[channel].size()<<' '<<state.pins[channel].size()<<'\n';
        for (const auto edge : state.seams[channel]) out<<edge<<'\n';
        for (const auto corner : state.pins[channel]) out<<corner<<'\n';
    }
    return out.str();
}
bool valid_uv_authoring(const CookedPolygonAsset& mesh,const PolygonUvAuthoring& state,std::string* error) {
    const auto fail=[&](std::string text) { if(error)*error=std::move(text); return false; };
    if (state.layoutHash!=polygon_uv_layout_hash(mesh))
        return fail("UV flags no longer match this triangle layout; Undo the topology edit or Reset UV Flags");
    const auto edges=polygon_all_components(mesh,PolygonSelectionMode::Edge);
    for (unsigned channel=0;channel<2;++channel) {
        if (state.seams[channel].size()>kMaximumUvSeams || state.pins[channel].size()>kMaximumUvPins)
            return fail("UV flags exceed the per-channel limits (40,000 seams, 60,000 pins)");
        for (const auto edge : state.seams[channel])
            if (!edges.contains(edge)) return fail("UV seam references a missing geometric edge");
        for (const auto corner : state.pins[channel])
            if (corner>=mesh.indices.size()) return fail("UV pin references a missing triangle corner");
    }
    return true;
}
}
std::optional<PolygonUvAuthoring> read_polygon_uv_authoring(const EditorObject& object,std::string* error) {
    const auto fail=[&](std::string text)->std::optional<PolygonUvAuthoring> {
        if(error)*error=std::move(text);
        return {};
    };
    if(error)error->clear();
    if(!object.polygon || !validate_polygon_asset(*object.polygon)) return fail("UV flags require a valid owned polygon");
    const Component* component=nullptr;
    for(const auto& candidate : object.components) if(candidate.type==kUvAuthoringComponent) {
        if(component) return fail("Multiple UV authoring records; use Reset UV Flags");
        component=&candidate;
    }
    PolygonUvAuthoring state;
    state.layoutHash=polygon_uv_layout_hash(*object.polygon);
    if(!component) return state;
    state.enabled=component->enabled;
    const auto property=component->properties.find("state");
    const auto* text=property==component->properties.end()?nullptr:std::get_if<std::string>(&property->second);
    if(!text || text->size()>kMaximumUvStateBytes) return fail("Invalid UV authoring record; use Reset UV Flags");
    if(text->empty())return state; // A newly added inspector component has no authored flags yet.
    std::istringstream input(*text);
    input.imbue(std::locale::classic());
    const auto number=[&](std::uint64_t& value) {
        std::string token;
        if(!(input>>token)) return false;
        const auto parsed=std::from_chars(token.data(),token.data()+token.size(),value);
        return parsed.ec==std::errc{} && parsed.ptr==token.data()+token.size();
    };
    std::string header;
    std::uint64_t version{},layout{},corners{};
    if(!(input>>header) || header!="DVE_UV" || !number(version) || version!=1 ||
       !number(layout) || !number(corners)) return fail("Invalid UV authoring header; use Reset UV Flags");
    if(layout!=state.layoutHash || corners!=object.polygon->indices.size())
        return fail("UV flags no longer match this triangle layout; Undo the topology edit or Reset UV Flags");
    for(unsigned channel=0;channel<2;++channel) {
        std::uint64_t seams{},pins{};
        if(!number(seams) || !number(pins) || seams>kMaximumUvSeams || pins>kMaximumUvPins)
            return fail("Invalid UV authoring counts; use Reset UV Flags");
        for(std::uint64_t i=0;i<seams;++i) {
            std::uint64_t edge{};
            if(!number(edge) || !state.seams[channel].insert(edge).second)
                return fail("Invalid or duplicate UV seam");
        }
        for(std::uint64_t i=0;i<pins;++i) {
            std::uint64_t corner{};
            if(!number(corner) || corner>=object.polygon->indices.size() ||
               !state.pins[channel].insert(std::uint32_t(corner)).second)
                return fail("Invalid or duplicate UV pin");
        }
    }
    input>>std::ws;
    if(!input.eof()) return fail("Unexpected data in UV authoring record");
    if(!valid_uv_authoring(*object.polygon,state,error)) return {};
    return state;
}
std::optional<PolygonUvAuthoring> remap_polygon_uv_authoring(const PolygonUvAuthoring& state,
    const CookedPolygonAsset& after,const PolygonTopologyLineage& lineage,std::string* error) {
    const auto fail=[&](std::string message)->std::optional<PolygonUvAuthoring> {
        if(error)*error=std::move(message); return {};
    };
    if (error) error->clear();
    if (const auto valid=validate_polygon_asset(after); !valid) return fail(valid.message);
    PolygonUvAuthoring mapped; mapped.enabled=state.enabled; mapped.layoutHash=polygon_uv_layout_hash(after);
    for (unsigned channel=0;channel<2;++channel) {
        if (state.seams[channel].size()>kMaximumUvSeams || state.pins[channel].size()>kMaximumUvPins)
            return fail("Source UV flags exceed their per-channel budget");
        for (auto seam : state.seams[channel]) {
            const auto at=lineage.edgeDescendants.find(seam);
            if (at==lineage.edgeDescendants.end() || at->second.empty())
                return fail("A saved UV seam has no cut lineage; triangulate the control face or clear that seam first");
            mapped.seams[channel].insert(at->second.begin(),at->second.end());
        }
        for (auto pin : state.pins[channel]) {
            const auto at=lineage.cornerDescendants.find(pin);
            if (at==lineage.cornerDescendants.end() || at->second.empty())
                return fail("A saved UV pin has no cut lineage; edit refused");
            mapped.pins[channel].insert(at->second.begin(),at->second.end());
        }
    }
    if (!valid_uv_authoring(after,mapped,error)) return {};
    return mapped;
}
std::optional<std::string> remap_polygon_uv_authoring_record(const EditorObject& object,
    const CookedPolygonAsset& after,const PolygonTopologyLineage& lineage,std::string* error) {
    const auto state=read_polygon_uv_authoring(object,error);
    if (!state) return {};
    if (!find_component_by_type(std::span<const Component>(object.components),kUvAuthoringComponent)) return std::string{};
    const auto mapped=remap_polygon_uv_authoring(*state,after,lineage,error);
    if (!mapped) return {};
    auto data=encode_uv_authoring(*mapped,after.indices.size());
    if (data.size()>kMaximumUvStateBytes) { if(error)*error="Remapped UV authoring record exceeds 4 MiB"; return {}; }
    return data;
}

std::unique_ptr<IEditorCommand> make_polygon_uv_authoring_command(EditorDocument& document,
    const EditorObject& object,const PolygonUvAuthoring& state,std::string* error) {
    if(!read_polygon_uv_authoring(object,error) || !valid_uv_authoring(*object.polygon,state,error)) return {};
    const auto data=encode_uv_authoring(state,object.polygon->indices.size());
    if(data.size()>kMaximumUvStateBytes) { if(error)*error="UV authoring record exceeds 4 MiB"; return {}; }
    if(const auto* component=find_component_by_type(std::span<const Component>(object.components),kUvAuthoringComponent)) {
        auto command=std::make_unique<CompoundCommand>("Edit UV seams and pins");
        command->add(std::make_unique<SetComponentPropertyCommand>(object.id,component->id,"state",
            component->properties.at("state"),ComponentValue{data},"Edit UV seams and pins"));
        if(component->enabled!=state.enabled)
            command->add(std::make_unique<SetComponentEnabledCommand>(object.id,component->id,
                component->enabled,state.enabled,"Enable UV seams and pins"));
        return command;
    }
    Component component;
    component.id=document.allocate_component_id(object.id);
    component.type=std::string(kUvAuthoringComponent);
    component.enabled=state.enabled;
    component.properties["state"]=data;
    return std::make_unique<AddComponentCommand>(object.id,std::move(component),"Create UV seams and pins");
}

namespace {
Float2 uv(const PolygonVertex& v,std::uint32_t channel){return channel?v.texcoord1:v.texcoord;}
void set_uv(PolygonVertex& v,std::uint32_t channel,Float2 p){if(channel)v.texcoord1=p;else v.texcoord=p;}
Float3 cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
using Point=std::tuple<float,float,float,float,float>;
Point identity(const PolygonVertex& v,std::uint32_t channel){auto p=uv(v,channel);return {v.position.x,v.position.y,v.position.z,p.x,p.y};}
// LSCM: weighted Cauchy-Riemann residuals in each triangle, with two fixed chart
// vertices. Matrix-free preconditioned CG solves A^T A; every solve is checked.
// Reference: Levy et al., SIGGRAPH 2002, https://brunolevy.github.io/papers/LSCM_SIGGRAPH_2002.pdf
bool unwrap_chart(CookedPolygonAsset& mesh,const std::set<std::uint32_t>& faces,std::uint32_t channel,std::string& error,const std::map<std::uint32_t,Float2>& pins,const std::set<std::uint64_t>& seams){
    if(faces.size()>20000) { error="UV chart exceeds the 20,000-face limit"; return false; }
    const auto geometric=polygon_geometric_vertices(mesh);
    std::map<std::uint32_t,std::size_t> local;
    std::vector<std::uint32_t> corners;
    for(const auto face : faces) for(unsigned j=0;j<3;++j) {
        const auto c=face*3+j;
        local.emplace(c,corners.size());
        corners.push_back(c);
    }
    std::vector<std::size_t> parent(corners.size());
    std::iota(parent.begin(),parent.end(),0);
    const auto root=[&](std::size_t node) {
        while(parent[node]!=node) { parent[node]=parent[parent[node]]; node=parent[node]; }
        return node;
    };
    const auto join=[&](std::uint32_t a,std::uint32_t b) {
        auto x=root(local.at(a)),y=root(local.at(b));
        if(x>y)std::swap(x,y);
        parent[y]=x;
    };
    std::map<std::uint64_t,std::vector<std::array<std::uint32_t,2>>> incidence;
    for(const auto face : faces) for(unsigned j=0;j<3;++j) {
        auto a=face*3+j,b=face*3+(j+1)%3;
        auto x=geometric[mesh.indices[a]],y=geometric[mesh.indices[b]];
        if(x>y) { std::swap(x,y); std::swap(a,b); }
        incidence[(std::uint64_t(x)<<32U)|y].push_back({a,b});
    }
    for(const auto& [edge,ends] : incidence) {
        if(ends.size()>2) { error="UV chart is nonmanifold"; return false; }
        if(ends.size()!=2 || seams.contains(edge)) continue;
        const auto a=ends[0],b=ends[1];
        if(identity(mesh.vertices[mesh.indices[a[0]]],channel)==identity(mesh.vertices[mesh.indices[b[0]]],channel) &&
           identity(mesh.vertices[mesh.indices[a[1]]],channel)==identity(mesh.vertices[mesh.indices[b[1]]],channel)) {
            join(a[0],b[0]);
            join(a[1],b[1]);
        }
    }
    std::map<std::size_t,std::size_t> nodes;
    std::vector<std::uint32_t> representatives;
    std::map<std::uint32_t,std::size_t> corner;
    for(const auto c : corners) {
        const auto [it,added]=nodes.emplace(root(local.at(c)),nodes.size());
        if(added) representatives.push_back(mesh.indices[c]);
        corner[c]=it->second;
    }
    std::map<std::pair<std::size_t,std::size_t>,unsigned> edges;
    const auto n=nodes.size();if(n>20000){error="UV chart exceeds the 20,000-vertex solve limit";return false;}
    for(auto f:faces)for(unsigned j=0;j<3;++j){auto a=corner[f*3+j],b=corner[f*3+(j+1)%3];++edges[std::minmax(a,b)];}
    bool boundary=false;for(auto [e,count]:edges){(void)e;if(count==1)boundary=true;else if(count!=2){error="UV chart is nonmanifold";return false;}}
    if(!boundary||std::int64_t(n)-std::int64_t(edges.size())+std::int64_t(faces.size())!=1){error="UV chart needs a disk boundary; split it into islands first";return false;}
    auto position=[&](auto i){return mesh.vertices[representatives[i]].position;};
    auto farthest=[&](auto start){std::size_t result=0;float distance=-1;for(std::size_t i=0;i<n;++i){auto d=length_squared(subtract(position(i),position(start)));if(d>distance){distance=d;result=i;}}return result;};
    auto anchorA=farthest(std::size_t(0)),anchorB=farthest(anchorA);double distance=length(subtract(position(anchorA),position(anchorB)));
    if(!(distance>1e-10)){error="UV chart has coincident anchors";return false;}
    std::map<std::size_t,Float2> pinned;
    for(auto [c,node]:corner){auto it=pins.find(c);if(it==pins.end())continue;auto [previous,added]=pinned.emplace(node,it->second);
        if(!added&&(std::abs(previous->second.x-it->second.x)>1e-6F||std::abs(previous->second.y-it->second.y)>1e-6F)){error="Shared UV point has conflicting pins";return false;}}
    if(pinned.empty()){pinned[anchorA]={0,0};pinned[anchorB]={float(distance),0};}
    else if(pinned.size()==1){auto [a,p]=*pinned.begin();auto b=farthest(a);pinned[b]={p.x+float(length(subtract(position(a),position(b)))),p.y};}
    const auto size=n*2;std::vector<int> free(size,-1);std::vector<double> fixed(size,0);std::vector<bool> locked(size);std::size_t variables=0;
    for(auto [node,p]:pinned){locked[node]=locked[node+n]=true;fixed[node]=p.x;fixed[node+n]=p.y;}
    for(std::size_t i=0;i<size;++i)if(!locked[i])free[i]=int(variables++);
    struct Row{std::vector<std::pair<std::size_t,double>> coefficients;double rhs{};};std::vector<Row> rows;
    for(auto f:faces){std::array<std::size_t,3> ids{corner[f*3],corner[f*3+1],corner[f*3+2]};
        auto p0=position(ids[0]),e1=subtract(position(ids[1]),p0),e2=subtract(position(ids[2]),p0);double L=length(e1);auto normal=cross(e1,e2);double area2=length(normal);
        if(!(L>1e-10&&area2>1e-14)){error="Cannot unwrap a degenerate triangle";return false;}
        double x=dot(e2,multiply(e1,float(1/L))),y=area2/L,weight=std::sqrt(area2*.5);
        std::array<double,3> gx{-y/(L*y),y/(L*y),0},gy{(x-L)/(L*y),-x/(L*y),1/y};
        for(unsigned equation=0;equation<2;++equation){Row row;for(unsigned j=0;j<3;++j)for(unsigned coord=0;coord<2;++coord){auto index=ids[j]+coord*n;double value=weight*(equation==0?(coord==0?gx[j]:-gy[j]):(coord==0?gy[j]:gx[j]));
                    if(free[index]>=0)row.coefficients.emplace_back(std::size_t(free[index]),value);else row.rhs-=value*fixed[index];}rows.push_back(std::move(row));}}
    std::vector<double> b(variables),diagonal(variables);for(const auto& row:rows)for(auto [i,a]:row.coefficients){b[i]+=a*row.rhs;diagonal[i]+=a*a;}
    auto apply=[&](const std::vector<double>& x){std::vector<double> out(variables);for(const auto& row:rows){double v=0;for(auto [i,a]:row.coefficients)v+=a*x[i];for(auto [i,a]:row.coefficients)out[i]+=a*v;}return out;};
    auto dot=[](const auto& a,const auto& b){return std::inner_product(a.begin(),a.end(),b.begin(),0.0);};
    std::vector<double> answer(variables),residual=b,z(variables),direction;for(std::size_t i=0;i<variables;++i){if(!(diagonal[i]>0)){error="UV chart is singular";return false;}z[i]=residual[i]/diagonal[i];}direction=z;double rz=dot(residual,z),initial=dot(b,b);bool converged=initial<1e-24;
    for(std::size_t iteration=0;!converged&&iteration<std::min<std::size_t>(variables*4+32,4000);++iteration){auto product=apply(direction);double denominator=dot(direction,product);if(!(denominator>0))break;double alpha=rz/denominator;
        for(std::size_t i=0;i<variables;++i){answer[i]+=alpha*direction[i];residual[i]-=alpha*product[i];}if(dot(residual,residual)<=std::max(1e-24,initial*1e-16)){converged=true;break;}
        for(std::size_t i=0;i<variables;++i)z[i]=residual[i]/diagonal[i];double next=dot(residual,z),beta=next/rz;for(std::size_t i=0;i<variables;++i)direction[i]=z[i]+beta*direction[i];rz=next;}
    if(!converged){error="UV solver did not converge; split or simplify the chart";return false;}
    std::vector<Float2> coordinates(n);for(std::size_t i=0;i<n;++i){double u=free[i]<0?fixed[i]:answer[std::size_t(free[i])],v=free[i+n]<0?fixed[i+n]:answer[std::size_t(free[i+n])];if(!std::isfinite(u)||!std::isfinite(v)){error="UV solve produced nonfinite coordinates";return false;}coordinates[i]={float(u),float(v)};}
    for(auto f:faces){auto a=coordinates[corner[f*3]],b=coordinates[corner[f*3+1]],c=coordinates[corner[f*3+2]];if((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)<=1e-12F){error="UV chart folds or collapses; add seams before unwrapping";return false;}}
    // A seam can cut a chart without splitting its face adjacency into separate islands.
    // Give each resulting corner sector its own attributes before assigning coordinates.
    std::map<std::uint32_t,std::size_t> owner;
    std::map<std::pair<std::uint32_t,std::size_t>,std::uint32_t> copies;
    for(const auto [c,node] : corner) {
        const auto index=mesh.indices[c];
        const auto [previous,added]=owner.emplace(index,node);
        if(!added && previous->second!=node) {
            const auto [copy,newCopy]=copies.emplace(std::pair{index,node},std::uint32_t(mesh.vertices.size()));
            if(newCopy) {
                if(mesh.vertices.size()>=1000000) { error="UV seam cut exceeds the geometry budget"; return false; }
                mesh.vertices.push_back(mesh.vertices[index]);
            }
            mesh.indices[c]=copy->second;
        }
        set_uv(mesh.vertices[mesh.indices[c]],channel,coordinates[node]);
    }
    return true;
}
}
std::vector<std::set<std::uint32_t>> polygon_uv_islands(const CookedPolygonAsset& mesh,std::uint32_t channel,const std::set<std::uint64_t>& seams){
    if(channel>1||!validate_polygon_asset(mesh))return {};
    const auto geometric=polygon_geometric_vertices(mesh);
    std::map<Point,std::uint32_t> points;std::vector<std::uint32_t> identityIds;for(const auto& v:mesh.vertices){auto [it,added]=points.emplace(identity(v,channel),std::uint32_t(points.size()));(void)added;identityIds.push_back(it->second);}
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::vector<std::uint32_t>> edges;
    for(std::uint32_t f=0;f<mesh.indices.size()/3;++f)for(unsigned j=0;j<3;++j){
        const auto ca=f*3+j,cb=f*3+(j+1)%3;
        const auto x=geometric[mesh.indices[ca]],y=geometric[mesh.indices[cb]];
        const auto key=(std::uint64_t(std::min(x,y))<<32U)|std::max(x,y);
        if(seams.contains(key))continue;
        auto a=identityIds[mesh.indices[ca]],b=identityIds[mesh.indices[cb]];
        edges[std::minmax(a,b)].push_back(f);
    }
    std::vector<std::vector<std::uint32_t>> neighbors(mesh.indices.size()/3);for(const auto& [e,fs]:edges){(void)e;if(fs.size()==2){neighbors[fs[0]].push_back(fs[1]);neighbors[fs[1]].push_back(fs[0]);}}
    std::set<std::uint32_t> remaining;for(std::uint32_t f=0;f<neighbors.size();++f)remaining.insert(f);std::vector<std::set<std::uint32_t>> islands;
    while(!remaining.empty()){std::queue<std::uint32_t> q;std::set<std::uint32_t> faces;q.push(*remaining.begin());remaining.erase(remaining.begin());while(!q.empty()){auto f=q.front();q.pop();faces.insert(f);for(auto next:neighbors[f])if(remaining.erase(next))q.push(next);}islands.push_back(std::move(faces));}return islands;
}
namespace {
// Area in the *authored object frame* with axis scaling applied; rotation
// preserves area, so no world transform is needed. Return double to avoid
// thin-chart rounding when measuring large meshes.
double triangle_metric_area(Float3 a,Float3 b,Float3 c,Float3 scale) {
    const auto u=subtract(multiply_components(b,scale),multiply_components(a,scale));
    const auto v=subtract(multiply_components(c,scale),multiply_components(a,scale));
    const double x=double(u.y)*v.z-double(u.z)*v.y;
    const double y=double(u.z)*v.x-double(u.x)*v.z;
    const double z=double(u.x)*v.y-double(u.y)*v.x;
    return .5*std::sqrt(x*x+y*y+z*z);
}
double triangle_uv_area(Float2 a,Float2 b,Float2 c) {
    return .5*std::abs((double(b.x)-a.x)*(double(c.y)-a.y)-
                         (double(b.y)-a.y)*(double(c.x)-a.x));
}
bool valid_density_settings(const UvEditSettings& s) {
    return s.channel<2 && s.textureWidth>0 && s.textureHeight>0 &&
           s.textureWidth<=16384 && s.textureHeight<=16384 &&
           std::isfinite(s.objectScale.x) && std::isfinite(s.objectScale.y) &&
           std::isfinite(s.objectScale.z) && s.objectScale.x>0 &&
           s.objectScale.y>0 && s.objectScale.z>0;
}
} // namespace

UvDensityReport measure_polygon_uv_density(const CookedPolygonAsset& mesh,
        const std::set<std::uint32_t>& corners,const UvEditSettings& settings) {
    UvDensityReport result;
    const auto fail=[&](std::string message) {
        result.charts.clear();result.error=std::move(message);return result;
    };
    if(!validate_polygon_asset(mesh) || corners.empty() || !valid_density_settings(settings))
        return fail("Select UV faces with a valid channel, object metric and texture size");
    std::set<std::uint32_t> selectedFaces;
    for(auto corner:corners) {
        if(corner>=mesh.indices.size())return fail("UV corner selection is stale");
        selectedFaces.insert(corner/3);
    }
    for(auto face:selectedFaces)for(unsigned i=0;i<3;++i)
        if(!corners.contains(face*3+i))
            return fail("Density works on complete UV faces; select an island or all corners");
    const auto islands=polygon_uv_islands(mesh,settings.channel,settings.seams);
    const double texels=double(settings.textureWidth)*settings.textureHeight;
    for(const auto& island:islands) {
        const bool selected=std::any_of(island.begin(),island.end(),
            [&](std::uint32_t face){return selectedFaces.contains(face);});
        if(!selected)continue;
        if(std::any_of(island.begin(),island.end(),
            [&](std::uint32_t face){return !selectedFaces.contains(face);}))
            return fail("Select whole UV islands for density measurement/matching");
        UvDensityChart chart;chart.faces=island.size();
        for(auto face:island) {
            const auto a=mesh.vertices[mesh.indices[face*3]];
            const auto b=mesh.vertices[mesh.indices[face*3+1]];
            const auto c=mesh.vertices[mesh.indices[face*3+2]];
            chart.geometricAreaSquareMeters+=triangle_metric_area(
                a.position,b.position,c.position,settings.objectScale);
            chart.uvArea+=triangle_uv_area(
                settings.channel==0?a.texcoord:a.texcoord1,
                settings.channel==0?b.texcoord:b.texcoord1,
                settings.channel==0?c.texcoord:c.texcoord1);
        }
        if(!(chart.geometricAreaSquareMeters>1e-16) || !(chart.uvArea>1e-16))
            return fail("UV chart is collapsed or has zero physical area; unwrap it first");
        chart.texelsPerMeter=std::sqrt(texels*chart.uvArea/chart.geometricAreaSquareMeters);
        if(!std::isfinite(chart.texelsPerMeter))return fail("UV density exceeds finite range");
        result.geometricAreaSquareMeters+=chart.geometricAreaSquareMeters;
        result.uvArea+=chart.uvArea;
        result.charts.push_back(chart);
    }
    if(result.charts.empty())return fail("No selected UV islands");
    result.texelsPerMeter=std::sqrt(texels*result.uvArea/result.geometricAreaSquareMeters);
    return result;
}

PolygonEditResult edit_polygon_uv(const CookedPolygonAsset& source,const std::set<std::uint32_t>& corners,UvEdit operation,const UvEditSettings& settings){
    PolygonEditResult r;r.mesh=source;r.selection={source.objectId,PolygonSelectionMode::Face,{}};
    auto fail=[&](std::string e){r.mesh=source;r.error=std::move(e);return r;};
    if(!validate_polygon_asset(source)||corners.empty()||settings.channel>1)return fail("Select UV corners in channel 0 or 1");
    for(auto c:corners)if(c>=source.indices.size())return fail("UV selection is stale");
    if(!std::isfinite(settings.vector.x)||!std::isfinite(settings.vector.y)||!std::isfinite(settings.pivot.x)||!std::isfinite(settings.pivot.y)||!std::isfinite(settings.angle)||!std::isfinite(settings.padding))return fail("UV tool values must be finite");
    for(auto [c,p]:settings.pins)if(c>=source.indices.size()||!std::isfinite(p.x)||!std::isfinite(p.y))return fail("Invalid UV pin");
    if(!settings.seams.empty()) {
        const auto edges=polygon_all_components(source,PolygonSelectionMode::Edge);
        for(const auto edge : settings.seams) if(!edges.contains(edge)) return fail("UV seam selection is stale");
    }
    if((operation==UvEdit::Unwrap||operation==UvEdit::Pack||operation==UvEdit::MatchDensity||
        operation==UvEdit::AlignIslandU||operation==UvEdit::AlignIslandV)&&corners.size()>60000)return fail("Select at most 20,000 UV faces per unwrap/pack operation");
    if(operation==UvEdit::Scale&&(!(settings.vector.x>0)||!(settings.vector.y>0)))return fail("UV scale factors must be positive");
    auto& m=r.mesh;std::set<std::uint32_t> outside;for(std::uint32_t c=0;c<source.indices.size();++c)if(!corners.contains(c))outside.insert(source.indices[c]);std::map<std::uint32_t,std::uint32_t> remap;
    for(auto c:corners){auto old=source.indices[c];auto [it,added]=remap.emplace(old,old);if(added&&outside.contains(old)){it->second=std::uint32_t(m.vertices.size());m.vertices.push_back(source.vertices[old]);}m.indices[c]=it->second;r.selection.components.insert(c/3);}
    std::set<std::uint32_t> selected;for(auto c:corners)selected.insert(m.indices[c]);
    if(operation==UvEdit::Unwrap||operation==UvEdit::Pack||operation==UvEdit::MatchDensity||
       operation==UvEdit::AlignIslandU||operation==UvEdit::AlignIslandV){
        std::set<std::uint32_t> faces;for(auto c:corners)faces.insert(c/3);for(auto f:faces)for(unsigned j=0;j<3;++j)if(!corners.contains(f*3+j))return fail("Select complete faces/islands for Unwrap or Pack");
        // Density and per-island alignment intentionally reject partial islands.
        if(operation==UvEdit::MatchDensity||operation==UvEdit::AlignIslandU||operation==UvEdit::AlignIslandV){
            const auto report=measure_polygon_uv_density(m,corners,settings);
            if(!report)return fail(report.error);
            if(operation==UvEdit::MatchDensity && (!(settings.targetTexelsPerMeter>0)||
               !std::isfinite(settings.targetTexelsPerMeter)))
               return fail("Target density must be a finite positive texels-per-metre value");
            for(const auto& [corner,pin] : settings.pins)
                if(corners.contains(corner))return fail("Selected UV island contains saved pins; clear pins before automatic layout");
        }
        std::vector<std::set<std::uint32_t>> charts;for(auto island:polygon_uv_islands(m,settings.channel,settings.seams)){std::set<std::uint32_t> chart;std::set_intersection(island.begin(),island.end(),faces.begin(),faces.end(),std::inserter(chart,chart.end()));if(!chart.empty())charts.push_back(std::move(chart));}
        std::map<std::uint32_t,std::size_t> owner;
        for(std::size_t chartIndex=0;chartIndex<charts.size();++chartIndex){std::map<std::uint32_t,std::uint32_t> copies;
            for(auto f:charts[chartIndex])for(unsigned j=0;j<3;++j){auto c=f*3+j,index=m.indices[c];auto [it,added]=owner.emplace(index,chartIndex);
                if(!added&&it->second!=chartIndex){auto [copy,newCopy]=copies.emplace(index,std::uint32_t(m.vertices.size()));if(newCopy)m.vertices.push_back(m.vertices[index]);m.indices[c]=copy->second;}}}
        if(operation==UvEdit::Unwrap){for(auto& chart:charts){std::string error;auto temporary=m;auto pinsCopy=settings.pins;if(!unwrap_chart(temporary,chart,settings.channel,error,pinsCopy,settings.seams)){
                if(error.find("disk boundary")==std::string::npos || !settings.seams.empty())return fail(error);
                for(auto face:chart){for(unsigned j=0;j<3;++j){auto c=face*3+j;auto v=m.vertices[m.indices[c]];m.indices[c]=std::uint32_t(m.vertices.size());m.vertices.push_back(v);}std::string triangleError;
                    if(!unwrap_chart(m,{face},settings.channel,triangleError,pinsCopy,settings.seams))return fail(triangleError);}
            }else m=std::move(temporary);}}
        else if(operation==UvEdit::MatchDensity||operation==UvEdit::AlignIslandU||operation==UvEdit::AlignIslandV) {
            for(const auto& chart:charts){
                std::set<std::uint32_t> indices;
                Float2 centre{};
                for(auto face:chart)for(unsigned i=0;i<3;++i)indices.insert(m.indices[face*3+i]);
                for(auto index:indices){auto p=uv(m.vertices[index],settings.channel);centre.x+=p.x;centre.y+=p.y;}
                centre.x/=float(indices.size());centre.y/=float(indices.size());
                double angle=0.0,scale=1.0;
                if(operation==UvEdit::MatchDensity){
                    std::set<std::uint32_t> chartCorners;
                    for(auto face:chart)for(unsigned i=0;i<3;++i)chartCorners.insert(face*3+i);
                    const auto density=measure_polygon_uv_density(m,chartCorners,settings);
                    if(!density)return fail(density.error);
                    scale=double(settings.targetTexelsPerMeter)/density.texelsPerMeter;
                    if(!(scale>0.0)||!std::isfinite(scale)||scale>1.0e7)
                        return fail("Requested density change exceeds the UV numeric budget");
                } else {
                    // Longest actual UV chart edge is a stable, artist-readable
                    // orientation reference. Rotating about the island centroid
                    // preserves its exact signed triangle areas and aspect ratio.
                    double best=0.0;Float2 direction{};
                    for(auto face:chart)for(unsigned i=0;i<3;++i){
                        auto a=uv(m.vertices[m.indices[face*3+i]],settings.channel);
                        auto b=uv(m.vertices[m.indices[face*3+(i+1)%3]],settings.channel);
                        const double dx=double(b.x)-a.x,dy=double(b.y)-a.y;
                        if(dx*dx+dy*dy>best){best=dx*dx+dy*dy;direction={float(dx),float(dy)};}
                    }
                    if(!(best>1.0e-20))return fail("UV island has no nonzero edge to align");
                    angle=(operation==UvEdit::AlignIslandV?1.5707963267948966:0.0)-
                        std::atan2(double(direction.y),double(direction.x));
                }
                const double cosine=std::cos(angle),sine=std::sin(angle);
                for(auto index:indices){
                    auto p=uv(m.vertices[index],settings.channel);
                    const double x=double(p.x)-centre.x,y=double(p.y)-centre.y;
                    p={float(double(centre.x)+scale*(x*cosine-y*sine)),
                       float(double(centre.y)+scale*(x*sine+y*cosine))};
                    if(!std::isfinite(p.x)||!std::isfinite(p.y))
                        return fail("Resulting UV coordinates overflowed");
                    set_uv(m.vertices[index],settings.channel,p);
                }
            }
        } else {
            if(!(settings.padding>=0&&settings.padding<.25F))return fail("UV padding must be between 0 and 0.25");
            auto grid=std::size_t(std::ceil(std::sqrt(double(charts.size()))));double cell=1.0/double(grid),margin=settings.padding;
            if(margin*2>=cell)return fail("Padding leaves no room for the selected island count");
            for(std::size_t i=0;i<charts.size();++i){std::set<std::uint32_t> vertices;Float2 minimum{INFINITY,INFINITY},maximum{-INFINITY,-INFINITY};
                for(auto f:charts[i])for(unsigned j=0;j<3;++j)vertices.insert(m.indices[f*3+j]);for(auto v:vertices){auto p=uv(m.vertices[v],settings.channel);minimum={std::min(minimum.x,p.x),std::min(minimum.y,p.y)};maximum={std::max(maximum.x,p.x),std::max(maximum.y,p.y)};}
                double extent=std::max(double(maximum.x-minimum.x),double(maximum.y-minimum.y));if(!(extent>1e-12))return fail("Cannot pack a collapsed UV island; unwrap it first");double scale=(cell-2*margin)/extent;
                for(auto v:vertices){auto p=uv(m.vertices[v],settings.channel);set_uv(m.vertices[v],settings.channel,{float(double(i%grid)*cell+margin+(p.x-minimum.x)*scale),float(double(i/grid)*cell+margin+(p.y-minimum.y)*scale)});}}
        }
    }else if(operation==UvEdit::StraightenU||operation==UvEdit::StraightenV) {
        for(const auto& [corner,pin]:settings.pins)
            if(corners.contains(corner))return fail("Selection includes saved UV pins; clear them before straightening");
        // Straighten U holds V fixed (horizontal line); Straighten V holds U
        // fixed (vertical line). Detached shared attributes protect outsiders.
        double sum=0.0;
        for(auto v:selected){const auto p=uv(m.vertices[v],settings.channel);
            sum+=operation==UvEdit::StraightenU?p.y:p.x;}
        const float constant=float(sum/double(selected.size()));
        for(auto v:selected){auto p=uv(m.vertices[v],settings.channel);
            if(operation==UvEdit::StraightenU)p.y=constant;else p.x=constant;
            set_uv(m.vertices[v],settings.channel,p);}
    }else if(operation==UvEdit::Stitch){
        std::map<std::tuple<float,float,float>,std::vector<std::uint32_t>> groups;for(auto v:selected){auto p=m.vertices[v].position;groups[{p.x,p.y,p.z}].push_back(v);}for(const auto& [p,vs]:groups){(void)p;Float2 average{};for(auto v:vs){auto x=uv(m.vertices[v],settings.channel);average.x+=x.x;average.y+=x.y;}average.x/=float(vs.size());average.y/=float(vs.size());for(auto v:vs)set_uv(m.vertices[v],settings.channel,average);}
    }else for(auto v:selected){auto p=uv(m.vertices[v],settings.channel);float x=p.x-settings.pivot.x,y=p.y-settings.pivot.y;
        if(operation==UvEdit::Translate||operation==UvEdit::Split){p.x+=settings.vector.x;p.y+=settings.vector.y;}
        else if(operation==UvEdit::Rotate){p={settings.pivot.x+x*std::cos(settings.angle)-y*std::sin(settings.angle),settings.pivot.y+x*std::sin(settings.angle)+y*std::cos(settings.angle)};}
        else if(operation==UvEdit::Scale){p={settings.pivot.x+x*settings.vector.x,settings.pivot.y+y*settings.vector.y};}
        else if(operation==UvEdit::FlipU)p.x=settings.pivot.x-x;else if(operation==UvEdit::FlipV)p.y=settings.pivot.y-y;
        set_uv(m.vertices[v],settings.channel,p);
    }
    if(m.vertices.size()>1000000)return fail("UV edit exceeds the geometry budget");refresh_editor_polygon(m);auto valid=validate_polygon_asset(m);if(!valid)return fail(valid.message);return r;
}
} // namespace dve::editor
