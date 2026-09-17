#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "dve/ai/mcp.hpp"
#include "dve/game_world.hpp"

int main(int argc,char** argv){
    std::filesystem::path root=std::filesystem::current_path();dve::ai::AiApprovalPolicy policy=dve::ai::AiApprovalPolicy::AskForChanges;
    for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--project-root"&&i+1<argc)root=argv[++i];else if(arg=="--read-only")policy=dve::ai::AiApprovalPolicy::ReadOnlyOnly;else if(arg=="--allow-changes")policy=dve::ai::AiApprovalPolicy::AllowChanges;else if(arg=="--help"){std::cout<<"dve_mcp_server [--project-root PATH] [--read-only|--allow-changes]\n";return 0;}}
    auto physics=std::make_unique<dve::ReferenceRigidBodyWorld>();dve::GameWorld world(std::move(physics));dve::ai::DveAiBridge bridge({root},&world);dve::ai::DveMcpProtocol protocol(bridge,policy);
    std::string line;while(std::getline(std::cin,line)){if(line.empty())continue;const std::string response=protocol.handle(line);if(!response.empty())std::cout<<response<<'\n'<<std::flush;}return 0;
}
