#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "dve/geometry_graph.hpp"

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

void test_editable_mesh(){
    dve::MeshBuilder builder;const auto a=builder.add_vertex({0,0,0}),b=builder.add_vertex({1,0,0}),c=builder.add_vertex({1,0,1}),d=builder.add_vertex({0,0,1});const auto face=builder.add_quad(a,d,c,b,3U);require(face.has_value(),"quad construction failed");auto mesh=std::move(builder).finish();
    require(mesh.vertex_count()==4U&&mesh.edge_count()==4U&&mesh.face_count()==1U&&mesh.corner_count()==4U,"topology counts are incorrect");std::string error;require(mesh.validate(&error),"topology validation failed");require(mesh.vertex_halfedges(a).size()==1U,"vertex adjacency failed");
    require(mesh.set_attribute(dve::MeshAttributeDomain::Point,"weight",a.index,0.5),"point attribute failed");require(mesh.set_attribute(dve::MeshAttributeDomain::Edge,"crease",0U,1.0),"edge attribute failed");require(mesh.set_attribute(dve::MeshAttributeDomain::Face,"walkable",face->index,true),"face attribute failed");require(mesh.set_attribute(dve::MeshAttributeDomain::Corner,"uv",0U,dve::Float2{0,0}),"corner attribute failed");require(mesh.set_detail_attribute("name",std::string("quad")),"detail attribute failed");require(mesh.attribute(dve::MeshAttributeDomain::Face,"walkable",face->index)!=nullptr,"attribute lookup failed");
    const auto cooked=dve::cooked_polygon_from_editable(mesh,99U,&error);require(error.empty()&&cooked.vertices.size()==4U&&cooked.indices.size()==6U,"editable to cooked conversion failed");const auto roundtrip=dve::editable_mesh_from_cooked(cooked,&error);require(error.empty()&&roundtrip.face_count()==2U,"cooked to editable conversion failed");
    const auto stale=*face;require(mesh.remove_face(*face),"face removal failed");require(!mesh.valid(stale),"generation-safe handle remained valid after deletion");
}

void test_graph(){
    dve::GeometryNodeRegistry registry;dve::register_builtin_geometry_nodes(registry);std::string error;
    int demandCounter=0;require(registry.register_node({"DemandCounter",1,{},{{"Geometry",dve::GeometrySocketType::Geometry,true}},[&](const auto&,std::string*)->dve::GeometryValue{++demandCounter;return dve::GeometrySet{};},{},true},&error),"counter node registration failed");
    dve::GeometryGraph graph;require(graph.add_node({1,"Grid",1,{{"xSegments",std::uint32_t{8}},{"zSegments",std::uint32_t{8}},{"size",dve::Float3{8,0,8}}}},&error),"grid node failed");require(graph.add_node({2,"Box",1,{{"size",dve::Float3{2,2,2}}}},&error),"box node failed");require(graph.add_node({3,"Transform",1,{{"translation",dve::Float3{0,1,0}}}},&error),"transform node failed");require(graph.add_node({4,"Join",1,{}},&error),"join node failed");require(graph.add_node({5,"MergeByDistance",1,{{"distance",0.0001}}},&error),"merge node failed");require(graph.add_node({6,"SetPosition",0,{{"translation",dve::Float3{1,0,0}}}},&error),"legacy set-position node failed");require(graph.add_node({7,"SetMaterial",1,{{"material",std::uint32_t{2}}}},&error),"material node failed");require(graph.add_node({99,"DemandCounter",1,{}},&error),"counter node failed");
    const auto path=std::filesystem::temp_directory_path()/"dve_geometry_graph_test.dmesh";require(graph.add_node({8,"BakeDmesh",1,{{"path",path.string()},{"objectId",std::uint32_t{231}}}},&error),"bake node failed");
    require(graph.connect({2,0,3,0},registry,&error),"box-transform link failed");require(graph.connect({1,0,4,0},registry,&error),"grid-join link failed");require(graph.connect({3,0,4,1},registry,&error),"transform-join link failed");require(graph.connect({4,0,5,0},registry,&error),"join-merge link failed");require(graph.connect({5,0,6,0},registry,&error),"merge-position link failed");require(graph.connect({6,0,7,0},registry,&error),"position-material link failed");require(graph.connect({7,0,8,0},registry,&error),"material-bake link failed");
    dve::GeometryGraph cycle;require(cycle.add_node({20,"Transform",1,{}},&error)&&cycle.add_node({21,"Transform",1,{}},&error),"cycle fixture failed");require(cycle.connect({20,0,21,0},registry,&error),"cycle fixture first link failed");require(!cycle.connect({21,0,20,0},registry,&error),"cycle was accepted");
    dve::GeometryGraphEvaluator evaluator(registry);const auto first=evaluator.evaluate(graph,{8,0,graph.revision(),nullptr});require(first.status==dve::GeometryEvaluationStatus::Success,"graph evaluation failed");require(std::filesystem::exists(path),"BakeDmesh did not create an asset");require(demandCounter==0,"demand-driven evaluation executed a disconnected node");const auto transformed=evaluator.evaluate(graph,{3,0,graph.revision(),nullptr});require(transformed.status==dve::GeometryEvaluationStatus::Success&&std::get<dve::GeometrySet>(transformed.value).instances.size()==1U,"Transform did not preserve first-class instances");const auto loaded=dve::read_dmesh(path);require(static_cast<bool>(loaded)&&loaded.asset.vertices.size()==85U&&loaded.asset.indices.size()==420U,"baked dmesh content is unexpected");
    const auto second=evaluator.evaluate(graph,{7,0,graph.revision(),nullptr});require(second.status==dve::GeometryEvaluationStatus::Success&&second.cacheHits>0U,"memory cache did not produce a hit");const auto oldHash=second.contentHash;require(graph.set_parameter(6,"translation",dve::Float3{2,0,0}),"parameter update failed");const auto changed=evaluator.evaluate(graph,{7,0,graph.revision(),nullptr});require(changed.status==dve::GeometryEvaluationStatus::Success&&changed.contentHash!=oldHash,"parameter hashing did not invalidate cached output");
    std::atomic_bool cancelled{true};require(evaluator.evaluate(graph,{7,0,graph.revision(),&cancelled}).status==dve::GeometryEvaluationStatus::Cancelled,"cancellation was ignored");require(evaluator.evaluate(graph,{7,0,graph.revision()+1U,nullptr}).status==dve::GeometryEvaluationStatus::Stale,"stale request was not rejected");
    std::error_code ec;std::filesystem::remove(path,ec);
}
}
int main(){try{test_editable_mesh();test_graph();std::cout<<"geometry graph tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<"geometry graph test failure: "<<e.what()<<'\n';return 1;}}
