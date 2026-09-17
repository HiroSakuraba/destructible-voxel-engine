#include "dve/geometry_graph.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>

namespace dve {
namespace {
std::uint64_t mix(std::uint64_t h,std::uint64_t v) noexcept{h^=v+0x9e3779b97f4a7c15ULL+(h<<6U)+(h>>2U);return h;}
std::uint64_t hash_string(std::string_view s) noexcept{std::uint64_t h=1469598103934665603ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}return h;}
std::uint64_t hash_value(const GeometryValue& value) noexcept{
    std::uint64_t h=mix(17U,value.index());
    std::visit([&](const auto& v){using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::monostate>){}else if constexpr(std::is_same_v<T,GeometrySet>)h=mix(h,geometry_set_content_hash(v));else if constexpr(std::is_same_v<T,double>)h=mix(h,std::bit_cast<std::uint64_t>(v));else if constexpr(std::is_same_v<T,Float3>){h=mix(h,std::bit_cast<std::uint32_t>(v.x));h=mix(h,std::bit_cast<std::uint32_t>(v.y));h=mix(h,std::bit_cast<std::uint32_t>(v.z));}else if constexpr(std::is_same_v<T,std::string>)h=mix(h,hash_string(v));else h=mix(h,static_cast<std::uint64_t>(v));},value);return h;
}
GeometrySocketType type_of(const GeometryValue& value){switch(value.index()){case 1:return GeometrySocketType::Geometry;case 2:return GeometrySocketType::Float;case 3:return GeometrySocketType::Float3;case 4:return GeometrySocketType::UInt;case 5:return GeometrySocketType::Bool;case 6:return GeometrySocketType::String;default:return GeometrySocketType::Geometry;}}
Float3 add(Float3 a,Float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Matrix4 translation(Float3 value){auto m=Matrix4::identity();m.values[12]=value.x;m.values[13]=value.y;m.values[14]=value.z;return m;}

template<typename T>
T parameter_or(const GeometryParameters& parameters,std::string_view name,T fallback){const auto it=parameters.find(std::string(name));if(it==parameters.end())return fallback;if(const auto* value=std::get_if<T>(&it->second))return *value;return fallback;}

void append_mesh(MeshBuilder& builder,const EditableMesh& mesh,const Matrix4& transform,std::uint32_t materialOverride=std::numeric_limits<std::uint32_t>::max()){
    std::unordered_map<std::uint32_t,VertexHandle> remap;
    for(const auto v:mesh.vertices())remap[v.index]=builder.add_vertex(transform_point(transform,mesh.vertex(v)->position));
    for(const auto f:mesh.faces()){
        std::vector<VertexHandle> verts;for(const auto v:mesh.face_vertices(f))verts.push_back(remap[v.index]);
        const auto material=materialOverride==std::numeric_limits<std::uint32_t>::max()?mesh.face(f)->material:materialOverride;
        (void)builder.add_polygon(verts,material);
    }
}

void realize_into(MeshBuilder& builder,const GeometrySet& geometry,const Matrix4& parent,std::uint32_t depth){
    if(depth>64U)return;
    if(geometry.mesh)append_mesh(builder,*geometry.mesh,parent);
    for(const auto& instance:geometry.instances)if(instance.geometry)realize_into(builder,*instance.geometry,multiply(parent,instance.transform),depth+1U);
}

GeometrySet box_node(const GeometryParameters& p){
    const Float3 size=parameter_or<Float3>(p,"size",{1,1,1});const Float3 half{size.x*0.5F,size.y*0.5F,size.z*0.5F};MeshBuilder b;
    const std::vector<Float3> points={{-half.x,-half.y,-half.z},{half.x,-half.y,-half.z},{half.x,half.y,-half.z},{-half.x,half.y,-half.z},{-half.x,-half.y,half.z},{half.x,-half.y,half.z},{half.x,half.y,half.z},{-half.x,half.y,half.z}};
    std::vector<VertexHandle> v;for(auto point:points)v.push_back(b.add_vertex(point));
    (void)b.add_quad(v[0],v[3],v[2],v[1]);(void)b.add_quad(v[4],v[5],v[6],v[7]);(void)b.add_quad(v[0],v[4],v[7],v[3]);(void)b.add_quad(v[1],v[2],v[6],v[5]);(void)b.add_quad(v[0],v[1],v[5],v[4]);(void)b.add_quad(v[3],v[7],v[6],v[2]);
    return {std::make_shared<EditableMesh>(std::move(b).finish()),{}};
}
GeometrySet grid_node(const GeometryParameters& p){
    const std::uint32_t x=std::max(1U,parameter_or<std::uint32_t>(p,"xSegments",1U));const std::uint32_t z=std::max(1U,parameter_or<std::uint32_t>(p,"zSegments",1U));const Float3 size=parameter_or<Float3>(p,"size",{1,0,1});MeshBuilder b;std::vector<VertexHandle> handles;handles.reserve(static_cast<std::size_t>(x+1U)*(z+1U));
    for(std::uint32_t iz=0;iz<=z;++iz)for(std::uint32_t ix=0;ix<=x;++ix){const float fx=static_cast<float>(ix)/static_cast<float>(x);const float fz=static_cast<float>(iz)/static_cast<float>(z);handles.push_back(b.add_vertex({(fx-0.5F)*size.x,0,(fz-0.5F)*size.z}));}
    const auto row=x+1U;for(std::uint32_t iz=0;iz<z;++iz)for(std::uint32_t ix=0;ix<x;++ix){const auto a=handles[iz*row+ix],bb=handles[iz*row+ix+1U],c=handles[(iz+1U)*row+ix+1U],d=handles[(iz+1U)*row+ix];(void)b.add_quad(a,d,c,bb);}
    return {std::make_shared<EditableMesh>(std::move(b).finish()),{}};
}
GeometrySet merge_distance(const GeometrySet& input,double distance){
    const auto realized=realize_geometry(input);if(!realized.mesh)return {};
    const double threshold=std::max(distance,0.0);MeshBuilder b;std::vector<Float3> unique;std::unordered_map<std::uint32_t,VertexHandle> remap;
    for(const auto v:realized.mesh->vertices()){
        const auto p=realized.mesh->vertex(v)->position;std::optional<std::size_t> found;
        for(std::size_t i=0;i<unique.size();++i){const double dx=static_cast<double>(p.x-unique[i].x),dy=static_cast<double>(p.y-unique[i].y),dz=static_cast<double>(p.z-unique[i].z);if(dx*dx+dy*dy+dz*dz<=threshold*threshold){found=i;break;}}
        if(found)remap[v.index]=VertexHandle{static_cast<std::uint32_t>(*found),1U};else{unique.push_back(p);remap[v.index]=b.add_vertex(p);}
    }
    for(const auto f:realized.mesh->faces()){std::vector<VertexHandle> verts;std::set<std::uint32_t> seen;for(const auto v:realized.mesh->face_vertices(f)){const auto h=remap[v.index];if(seen.insert(h.index).second)verts.push_back(h);}if(verts.size()>=3U)(void)b.add_polygon(verts,realized.mesh->face(f)->material);}
    return {std::make_shared<EditableMesh>(std::move(b).finish()),{}};
}
}

GeometrySet realize_geometry(const GeometrySet& geometry){MeshBuilder b;realize_into(b,geometry,Matrix4::identity(),0U);auto mesh=std::make_shared<EditableMesh>(std::move(b).finish());if(mesh->face_count()==0U&&mesh->vertex_count()==0U)return {};return {std::move(mesh),{}};}
std::uint64_t geometry_set_content_hash(const GeometrySet& geometry) noexcept{std::uint64_t h=17U;if(geometry.mesh)h=mix(h,editable_mesh_content_hash(*geometry.mesh));for(const auto& i:geometry.instances){h=mix(h,i.stableId);for(float v:i.transform.values)h=mix(h,std::bit_cast<std::uint32_t>(v));if(i.geometry)h=mix(h,geometry_set_content_hash(*i.geometry));}return h;}

bool GeometryNodeRegistry::register_node(GeometryNodeDefinition definition,std::string* error){if(definition.type.empty()||!definition.evaluate){if(error)*error="node definition requires a type and evaluator";return false;}if(definitions_.contains(definition.type)){if(error)*error="node type is already registered";return false;}definitions_.emplace(definition.type,std::move(definition));return true;}
const GeometryNodeDefinition* GeometryNodeRegistry::find(std::string_view type) const noexcept{const auto it=definitions_.find(std::string(type));return it==definitions_.end()?nullptr:&it->second;}

bool GeometryGraph::add_node(GeometryNodeInstance node,std::string* error){if(node.id==0U||node.type.empty()||nodes_.contains(node.id)){if(error)*error="node id and type must be unique and nonzero";return false;}nodes_.emplace(node.id,std::move(node));++revision_;return true;}
bool GeometryGraph::remove_node(GeometryNodeId id){if(nodes_.erase(id)==0U)return false;std::erase_if(links_,[&](const auto& l){return l.source==id||l.destination==id;});++revision_;return true;}
const GeometryNodeInstance* GeometryGraph::node(GeometryNodeId id) const noexcept{const auto it=nodes_.find(id);return it==nodes_.end()?nullptr:&it->second;}
bool GeometryGraph::set_parameter(GeometryNodeId id,std::string name,GeometryValue value){auto it=nodes_.find(id);if(it==nodes_.end()||name.empty())return false;it->second.parameters[std::move(name)]=std::move(value);++revision_;return true;}
bool GeometryGraph::connect(GeometryLink link,const GeometryNodeRegistry& registry,std::string* error){
    const auto* source=node(link.source);const auto* destination=node(link.destination);if(!source||!destination){if(error)*error="link references an unknown node";return false;}const auto* sd=registry.find(source->type);const auto* dd=registry.find(destination->type);if(!sd||!dd||link.sourceSocket>=sd->outputs.size()||link.destinationSocket>=dd->inputs.size()){if(error)*error="link references an unknown socket";return false;}if(sd->outputs[link.sourceSocket].type!=dd->inputs[link.destinationSocket].type){if(error)*error="socket types do not match";return false;}for(const auto& l:links_)if(l.destination==link.destination&&l.destinationSocket==link.destinationSocket){if(error)*error="input socket already has a link";return false;}links_.push_back(link);++revision_;std::string validation;if(!validate(registry,&validation)){links_.pop_back();--revision_;if(error)*error=validation;return false;}return true;
}
bool GeometryGraph::validate(const GeometryNodeRegistry& registry,std::string* error) const{
    auto fail=[&](std::string m){if(error)*error=std::move(m);return false;};
    for(const auto& [id,n]:nodes_){(void)id;const auto* d=registry.find(n.type);if(!d)return fail("graph references an unregistered node type");if(n.schemaVersion>d->schemaVersion)return fail("node schema is newer than the registry definition");}
    std::unordered_map<GeometryNodeId,std::vector<GeometryNodeId>> adjacency;for(const auto& l:links_){const auto* s=node(l.source);const auto* d=node(l.destination);if(!s||!d)return fail("graph link references an unknown node");const auto* sd=registry.find(s->type);const auto* dd=registry.find(d->type);if(!sd||!dd||l.sourceSocket>=sd->outputs.size()||l.destinationSocket>=dd->inputs.size())return fail("graph link references an unknown socket");if(sd->outputs[l.sourceSocket].type!=dd->inputs[l.destinationSocket].type)return fail("graph link has incompatible socket types");adjacency[l.source].push_back(l.destination);}
    std::unordered_map<GeometryNodeId,int> color;std::function<bool(GeometryNodeId)> visit=[&](GeometryNodeId id){if(color[id]==1)return false;if(color[id]==2)return true;color[id]=1;for(auto next:adjacency[id])if(!visit(next))return false;color[id]=2;return true;};for(const auto& [id,n]:nodes_){(void)n;if(!visit(id))return fail("geometry graph contains a cycle");}return true;
}

GeometryEvaluationResult GeometryGraphEvaluator::evaluate(const GeometryGraph& graph,const GeometryEvaluationRequest& request){
    GeometryEvaluationResult result;if(request.cancelled&&request.cancelled->load()){result.status=GeometryEvaluationStatus::Cancelled;return result;}if(request.expectedGraphRevision!=0U&&request.expectedGraphRevision!=graph.revision()){result.status=GeometryEvaluationStatus::Stale;result.error="graph revision changed before evaluation";return result;}std::string validation;if(!graph.validate(registry_,&validation)){result.status=GeometryEvaluationStatus::InvalidGraph;result.error=validation;return result;}
    std::unordered_map<GeometryNodeId,CachedValue> local;std::unordered_set<GeometryNodeId> active;
    std::function<std::optional<CachedValue>(GeometryNodeId,std::uint32_t)> eval=[&](GeometryNodeId id,std::uint32_t output)->std::optional<CachedValue>{
        if(request.cancelled&&request.cancelled->load()){result.status=GeometryEvaluationStatus::Cancelled;return std::nullopt;}if(request.expectedGraphRevision!=0U&&request.expectedGraphRevision!=graph.revision()){result.status=GeometryEvaluationStatus::Stale;return std::nullopt;}if(const auto it=local.find(id);it!=local.end())return it->second;if(!active.insert(id).second){result.error="cycle encountered during evaluation";return std::nullopt;}
        const auto* node=graph.node(id);if(!node){result.error="requested node does not exist";active.erase(id);return std::nullopt;}const auto* def=registry_.find(node->type);if(!def||output>=def->outputs.size()){result.error="requested node output is invalid";active.erase(id);return std::nullopt;}
        GeometryParameters params=node->parameters;if(node->schemaVersion<def->schemaVersion){if(!def->migrate){result.error="node requires schema migration";active.erase(id);return std::nullopt;}std::string migrationError;if(!def->migrate(node->schemaVersion,params,&migrationError)){result.error=migrationError;active.erase(id);return std::nullopt;}}
        std::vector<GeometryValue> inputs(def->inputs.size());std::uint64_t key=hash_string(def->type);key=mix(key,def->schemaVersion);std::vector<std::pair<std::string,GeometryValue>> sorted(params.begin(),params.end());std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.first<b.first;});for(const auto& [name,value]:sorted){key=mix(key,hash_string(name));key=mix(key,hash_value(value));}
        for(std::size_t socket=0;socket<def->inputs.size();++socket){const auto it=std::find_if(graph.links().begin(),graph.links().end(),[&](const auto& l){return l.destination==id&&l.destinationSocket==socket;});if(it==graph.links().end()){if(def->inputs[socket].required){result.error="required input is not connected: "+def->inputs[socket].name;active.erase(id);return std::nullopt;}continue;}const auto upstream=eval(it->source,it->sourceSocket);if(!upstream){active.erase(id);return std::nullopt;}inputs[socket]=upstream->value;key=mix(key,upstream->hash);}
        if(def->pure){if(const auto cached=cache_.find(key);cached!=cache_.end()){++result.cacheHits;local[id]=cached->second;active.erase(id);return cached->second;}}
        std::string evaluationError;GeometryValue value=def->evaluate({inputs,params,request.cancelled},&evaluationError);if(!evaluationError.empty()){result.error=evaluationError;active.erase(id);return std::nullopt;}if(type_of(value)!=def->outputs[output].type){result.error="node returned a value with the wrong socket type";active.erase(id);return std::nullopt;}CachedValue cv{std::move(value),mix(key,hash_value(value))};if(def->pure)cache_[key]=cv;local[id]=cv;++result.evaluatedNodes;active.erase(id);return cv;
    };
    const auto value=eval(request.node,request.outputSocket);if(!value){if(result.status!=GeometryEvaluationStatus::Cancelled&&result.status!=GeometryEvaluationStatus::Stale)result.status=GeometryEvaluationStatus::Failed;return result;}result.value=value->value;result.contentHash=value->hash;result.status=GeometryEvaluationStatus::Success;return result;
}

void register_builtin_geometry_nodes(GeometryNodeRegistry& r){
    const GeometrySocketDefinition geoIn{"Geometry",GeometrySocketType::Geometry,true};const GeometrySocketDefinition geoOut{"Geometry",GeometrySocketType::Geometry,true};std::string ignored;
    (void)r.register_node({"Box",1,{}, {geoOut},[](const auto& c,std::string*)->GeometryValue{return box_node(c.parameters);},{},true},&ignored);
    (void)r.register_node({"Grid",1,{}, {geoOut},[](const auto& c,std::string*)->GeometryValue{return grid_node(c.parameters);},{},true},&ignored);
    (void)r.register_node({"Transform",1,{geoIn},{geoOut},[](const auto& c,std::string*)->GeometryValue{const auto* input=std::get_if<GeometrySet>(&c.inputs[0]);if(!input)return GeometrySet{};auto shared=std::make_shared<GeometrySet>(*input);GeometrySet out;out.instances.push_back({std::move(shared),translation(parameter_or<Float3>(c.parameters,"translation",{})),parameter_or<std::uint32_t>(c.parameters,"stableId",0U)});return out;},{},true},&ignored);
    (void)r.register_node({"Join",1,{{"A",GeometrySocketType::Geometry,true},{"B",GeometrySocketType::Geometry,true}},{geoOut},[](const auto& c,std::string*)->GeometryValue{GeometrySet out;for(const auto& input:c.inputs)if(const auto* g=std::get_if<GeometrySet>(&input))out.instances.push_back({std::make_shared<GeometrySet>(*g),Matrix4::identity(),static_cast<std::uint64_t>(out.instances.size()+1U)});return out;},{},true},&ignored);
    (void)r.register_node({"MergeByDistance",1,{geoIn},{geoOut},[](const auto& c,std::string*)->GeometryValue{const auto* input=std::get_if<GeometrySet>(&c.inputs[0]);return input?GeometryValue{merge_distance(*input,parameter_or<double>(c.parameters,"distance",0.001))}:GeometryValue{GeometrySet{}};},{},true},&ignored);
    (void)r.register_node({"SetPosition",1,{geoIn},{geoOut},[](const auto& c,std::string*)->GeometryValue{const auto* input=std::get_if<GeometrySet>(&c.inputs[0]);if(!input)return GeometrySet{};auto out=realize_geometry(*input);const auto offset=parameter_or<Float3>(c.parameters,"offset",{});if(out.mesh)for(const auto v:out.mesh->vertices())out.mesh->vertex(v)->position=add(out.mesh->vertex(v)->position,offset);return out;},[](std::uint32_t old,GeometryParameters& p,std::string*){if(old==0U){const auto it=p.find("translation");if(it!=p.end()){p["offset"]=it->second;p.erase(it);}return true;}return old==1U;},true},&ignored);
    (void)r.register_node({"SetMaterial",1,{geoIn},{geoOut},[](const auto& c,std::string*)->GeometryValue{const auto* input=std::get_if<GeometrySet>(&c.inputs[0]);if(!input)return GeometrySet{};auto out=realize_geometry(*input);const auto material=parameter_or<std::uint32_t>(c.parameters,"material",0U);if(out.mesh)for(const auto f:out.mesh->faces()){out.mesh->face(f)->material=material;}return out;},{},true},&ignored);
    (void)r.register_node({"BakeDmesh",1,{geoIn},{geoOut},[](const auto& c,std::string* error)->GeometryValue{const auto* input=std::get_if<GeometrySet>(&c.inputs[0]);if(!input){if(error)*error="BakeDmesh requires geometry";return GeometrySet{};}const auto path=parameter_or<std::string>(c.parameters,"path","");if(path.empty()){if(error)*error="BakeDmesh requires a path";return GeometrySet{};}auto realized=realize_geometry(*input);std::string cookError;const auto cooked=realized.mesh?cooked_polygon_from_editable(*realized.mesh,parameter_or<std::uint32_t>(c.parameters,"objectId",1U),&cookError):CookedPolygonAsset{};if(!cookError.empty()||!write_dmesh(std::filesystem::path(path),cooked,&cookError)){if(error)*error=cookError.empty()?"failed to write dmesh":cookError;}return realized;},{},false},&ignored);
}

} // namespace dve
