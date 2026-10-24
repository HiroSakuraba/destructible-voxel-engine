#include "dve/render/material_texture_bake.hpp"
#include "dve/transform.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <locale>
#include <algorithm>
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
    if(argc<3){std::cerr<<"Usage: dve_bake_material source.dmesh NEW_BUNDLE_DIR [--material N] [--uv 0|1] [--resolution N] [--samples N] [--padding N] [--maps MASK] [--budget-mib N] [--meters-per-unit F]\nMaps: base=1 normal=2 MR=4 emission=8 opacity=16; default=15. AO=32 diffuse-lighting-RGBM=64. --atlas 1 generates UV1; --all-materials 1; --texels-per-meter F; --ray-samples N; --ao-distance F; --lighting-range F; --bounces N (0..8); --settings FILE; --scene BUNDLE_DIR (frozen neighbours).\n";return 2;}
    try{
        MaterialTextureBakeSettings settings;MaterialBakeScene scene;
        std::filesystem::path sceneDirectory;
        for(int n=3;n<argc;++n){std::string key=argv[n];if(++n==argc)throw std::runtime_error("Missing option value");std::string value=argv[n];
            if(key=="--scene"){sceneDirectory=value;continue;}
            if(key=="--settings"){std::ifstream file(value,std::ios::binary);if(!file)throw std::runtime_error("Cannot open settings");std::ostringstream text;text<<file.rdbuf();if(!decode_material_bake_settings(text.str(),settings))throw std::runtime_error("Invalid settings snapshot");continue;}
            if(key=="--texels-per-meter"||key=="--ao-distance"||key=="--lighting-range"){std::size_t used{};const float f=std::stof(value,&used);if(used!=value.size())throw std::runtime_error("Invalid float");if(key=="--texels-per-meter")settings.texelsPerMeter=f;else if(key=="--ao-distance")settings.aoDistanceMeters=f;else settings.lightingRange=f;continue;}
            if(key=="--materials"){settings.materialSlots.clear();std::istringstream slots(value);std::string slot;while(std::getline(slots,slot,',')){unsigned n0{};auto p0=std::from_chars(slot.data(),slot.data()+slot.size(),n0);if(p0.ec!=std::errc{}||p0.ptr!=slot.data()+slot.size())throw std::runtime_error("Invalid slot list");settings.materialSlots.push_back(n0);}continue;}
            if(key=="--meters-per-unit"){std::size_t used{};settings.metersPerUnit=std::stof(value,&used);if(used!=value.size())throw std::runtime_error("Invalid scale");continue;}
            unsigned number{};auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())throw std::runtime_error("Expected unsigned integer for "+key);
            if(key=="--atlas"){if(number>1)throw std::runtime_error("Atlas flag must be 0 or 1");settings.generateAtlas=number!=0;if(settings.generateAtlas)settings.targetUv=1;}
            else if(key=="--all-materials"){if(number>1)throw std::runtime_error("All-materials flag must be 0 or 1");settings.allAssignedSlots=number!=0;}
            else if(key=="--ray-samples")settings.raySamples=number;else if(key=="--seed")settings.seed=number;else if(key=="--bounces")settings.lighting.pathTraceBounces=number;
            else if(key=="--material")settings.materialIndex=number;else if(key=="--uv")settings.targetUv=number;else if(key=="--resolution")settings.resolution=number;
            else if(key=="--samples")settings.samplesPerAxis=number;else if(key=="--padding")settings.paddingPixels=number;else if(key=="--maps")settings.maps=number;
            else if(key=="--budget-mib")settings.memoryBudgetBytes=std::uint64_t(number)*1048576;else throw std::runtime_error("Unknown option: "+key);
        }
        auto source=read_dmesh(argv[1],settings.memoryBudgetBytes);if(!source)throw std::runtime_error(source.error);
        if(!sceneDirectory.empty()){
            std::ifstream poses(sceneDirectory/"scene.txt");poses.imbue(std::locale::classic());unsigned count{};if(!(poses>>count)||count>4096)throw std::runtime_error("Invalid scene snapshot count");std::uint64_t bytes=0;
            for(unsigned n=0;n<count;++n){MaterialBakeSceneMesh m;poses>>m.pose.position.x>>m.pose.position.y>>m.pose.position.z>>m.pose.rotation.x>>m.pose.rotation.y>>m.pose.rotation.z>>m.pose.rotation.w;
                const auto file=sceneDirectory/("scene-"+std::to_string(n)+".dmesh");const auto size=std::filesystem::file_size(file);if(!poses||size>settings.memoryBudgetBytes-bytes)throw std::runtime_error("Invalid or oversized scene snapshot");bytes+=size;
                auto mesh=read_dmesh(file,settings.memoryBudgetBytes);if(!mesh)throw std::runtime_error(mesh.error);m.asset=std::move(mesh.asset);scene.push_back(std::move(m));
            }poses>>std::ws;if(!poses.eof())throw std::runtime_error("Trailing scene snapshot data");
        }
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);std::atomic<bool> cancel{};
        std::jthread watcher([&](std::stop_token token){while(!token.stop_requested()){if(interrupted)cancel=true;std::this_thread::sleep_for(std::chrono::milliseconds(20));}});
        MaterialBakeControl control{&cancel,[&](float progress,std::string_view phase){if(interrupted)cancel=true;std::cerr<<int(progress*100)<<"% "<<phase<<'\n';}};
        auto bake=bake_material_textures(source.asset,settings,control,scene);if(interrupted)cancel=true;
        if(!bake.success)throw std::runtime_error(bake.error);
        std::string error;if(!write_material_bake_bundle(argv[2],source.asset,bake,control,&error,scene))throw std::runtime_error(error);
        std::cout<<"Exported "<<argv[2]<<" ("<<bake.coveredPixels<<" covered pixels, "<<bake.chartCount<<" charts)\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return interrupted?130:1;}
}
