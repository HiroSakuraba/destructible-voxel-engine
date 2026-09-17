#include <filesystem>
#include <iostream>
#include <string>

#include "dve/geometry_graph.hpp"

int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"usage: dve_geometry_graph_bake <output.dmesh>\n";return 2;}
    const std::filesystem::path output=argv[1];dve::GeometryNodeRegistry registry;dve::register_builtin_geometry_nodes(registry);dve::GeometryGraph graph;std::string error;
    const auto add=[&](dve::GeometryNodeInstance n){if(!graph.add_node(std::move(n),&error)){std::cerr<<error<<'\n';return false;}return true;};
    if(!add({1,"Grid",1,{{"xSegments",std::uint32_t{8}},{"zSegments",std::uint32_t{8}},{"size",dve::Float3{8,0,8}}}})||!add({2,"Box",1,{{"size",dve::Float3{2,2,2}}}})||!add({3,"Transform",1,{{"translation",dve::Float3{0,1,0}}}})||!add({4,"Join",1,{}})||!add({5,"BakeDmesh",1,{{"path",output.string()},{"objectId",std::uint32_t{231}}}}))return 1;
    for(const auto link:{dve::GeometryLink{2,0,3,0},dve::GeometryLink{1,0,4,0},dve::GeometryLink{3,0,4,1},dve::GeometryLink{4,0,5,0}})if(!graph.connect(link,registry,&error)){std::cerr<<error<<'\n';return 1;}
    dve::GeometryGraphEvaluator evaluator(registry);const auto result=evaluator.evaluate(graph,{5,0,graph.revision(),nullptr});if(result.status!=dve::GeometryEvaluationStatus::Success){std::cerr<<result.error<<'\n';return 1;}const auto loaded=dve::read_dmesh(output);if(!loaded){std::cerr<<loaded.error<<'\n';return 1;}std::cout<<"baked "<<output<<" vertices="<<loaded.asset.vertices.size()<<" triangles="<<loaded.asset.indices.size()/3U<<" hash="<<loaded.asset.contentHash<<'\n';return 0;
}
