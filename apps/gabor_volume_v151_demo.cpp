#include "dve/gabor_volume.hpp"
#include "dve/render/gabor_volume_renderer.hpp"
#include "dve/rhi/null_device.hpp"
#include "dve/runtime_gabor_volume_world.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv){
    try{
        if(argc<3)throw std::runtime_error("usage: dve_gabor_volume_v151_demo ASSET.dgabor OUTPUT_DIR");
        const std::filesystem::path assetPath=argv[1],output=argv[2];std::filesystem::create_directories(output);
        auto loaded=dve::read_dgabor(assetPath);if(!loaded)throw std::runtime_error(loaded.error);
        auto& asset=*loaded.asset;std::string error;
        dve::RuntimeGaborVolumeWorld world;const auto object=world.create(asset,{},151,asset.contentHash,&error);if(!object)throw std::runtime_error(error);
        dve::rhi::NullDevice device;dve::render::GaborGpuCache cache(device);if(!cache.upload(asset.contentHash,asset,&error))throw std::runtime_error(error);
        dve::GaborVolumeRenderSettings settings;settings.quality=dve::GaborVolumeQuality::Medium;settings.maximumPrimitivesPerTile=256;
        dve::render::GaborVolumeHistoryState history;auto snapshot=world.snapshot();const auto plan=dve::render::make_gabor_volume_render_plan(snapshot,cache,settings,128,72,128.0F,1,&history);
        if(!plan.validate(&error))throw std::runtime_error(error);
        dve::GaborPreviewOptions preview;preview.width=128;preview.height=72;preview.raySteps=24;
        if(!dve::render_gabor_preview_ppm(output/"gabor_volume_v1_51.ppm",asset,preview,&error))throw std::runtime_error(error);
        std::uint16_t maxLod=0;for(const auto&p:asset.primitives)maxLod=std::max(maxLod,p.lodLevel);
        std::ofstream evidence(output/"gabor_volume_v1_51_evidence.json");
        evidence<<"{\n"
                <<"  \"version\": \"1.51.0\",\n"
                <<"  \"asset\": \""<<assetPath.filename().generic_string()<<"\",\n"
                <<"  \"content_hash\": \""<<std::hex<<asset.contentHash<<std::dec<<"\",\n"
                <<"  \"primitives\": "<<asset.primitives.size()<<",\n"
                <<"  \"lod_levels\": "<<(static_cast<unsigned>(maxLod)+1U)<<",\n"
                <<"  \"submitted_primitives\": "<<plan.submittedPrimitives<<",\n"
                <<"  \"culled_primitives\": "<<plan.culledPrimitives<<",\n"
                <<"  \"dispatches\": "<<plan.dispatches.size()<<",\n"
                <<"  \"temporal_history_reset\": "<<(plan.temporalHistoryReset?"true":"false")<<",\n"
                <<"  \"volume_shadow_work\": "<<(plan.volumeShadowWork?"true":"false")<<",\n"
                <<"  \"resident_bytes\": "<<cache.stats().residentBytes<<"\n"
                <<"}\n";
        if(!evidence)throw std::runtime_error("could not write demonstration evidence");
        std::cout<<output<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"DVE v1.51 Gabor demo failed: "<<e.what()<<'\n';return 1;}
}
