#include "dve/render/material_texture_bake.hpp"
#include "dve/transform.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
using namespace dve;
using namespace dve::render;
namespace {volatile std::sig_atomic_t interrupted=0;void stop(int){interrupted=1;}}
int main(int argc,char** argv){
    if(argc<3){std::cerr<<"Usage: dve_bake_material source.dmesh NEW_BUNDLE_DIR [--material N] [--uv 0|1] [--resolution N] [--samples N] [--padding N] [--maps MASK] [--budget-mib N] [--meters-per-unit F]\nMaps: base=1 normal=2 MR=4 emission=8 opacity=16; default=15. No lighting.\n";return 2;}
    try{
        MaterialTextureBakeSettings settings;
        for(int n=3;n<argc;++n){std::string key=argv[n];if(++n==argc)throw std::runtime_error("Missing option value");std::string value=argv[n];
            if(key=="--meters-per-unit"){std::size_t used{};settings.metersPerUnit=std::stof(value,&used);if(used!=value.size())throw std::runtime_error("Invalid scale");continue;}
            unsigned number{};auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())throw std::runtime_error("Expected unsigned integer for "+key);
            if(key=="--material")settings.materialIndex=number;else if(key=="--uv")settings.targetUv=number;else if(key=="--resolution")settings.resolution=number;
            else if(key=="--samples")settings.samplesPerAxis=number;else if(key=="--padding")settings.paddingPixels=number;else if(key=="--maps")settings.maps=number;
            else if(key=="--budget-mib")settings.memoryBudgetBytes=std::uint64_t(number)*1048576;else throw std::runtime_error("Unknown option: "+key);
        }
        auto source=read_dmesh(argv[1],settings.memoryBudgetBytes);if(!source)throw std::runtime_error(source.error);
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);std::atomic<bool> cancel{};
        std::jthread watcher([&](std::stop_token token){while(!token.stop_requested()){if(interrupted)cancel=true;std::this_thread::sleep_for(std::chrono::milliseconds(20));}});
        MaterialBakeControl control{&cancel,[&](float progress,std::string_view phase){if(interrupted)cancel=true;std::cerr<<int(progress*100)<<"% "<<phase<<'\n';}};
        auto bake=bake_material_textures(source.asset,settings,control);if(interrupted)cancel=true;
        if(!bake.success)throw std::runtime_error(bake.error);
        std::string error;if(!write_material_bake_bundle(argv[2],source.asset,bake,control,&error))throw std::runtime_error(error);
        std::cout<<"Exported "<<argv[2]<<" ("<<bake.coveredPixels<<" covered pixels, "<<bake.chartCount<<" charts)\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return interrupted?130:1;}
}
