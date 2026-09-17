#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "dve/gabor_volume.hpp"

namespace {
void require(bool condition,std::string_view message){if(!condition)throw std::runtime_error(std::string(message));}
}

int main(){
    try{
        const auto root=std::filesystem::temp_directory_path()/"dve_gabor_volume_tests";
        std::filesystem::remove_all(root);std::filesystem::create_directories(root);
        const auto ply=root/"root.primitives_pyr0.ply";
        std::ofstream p(ply);
        p<<"ply\nformat ascii 1.0\nelement vertex 2\n"
          <<"property float x\nproperty float y\nproperty float z\n"
          <<"property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
          <<"property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
          <<"property float opacities\nproperty float omega\nproperty float extent\n"
          <<"property float albedo_0\nproperty float albedo_1\nproperty float albedo_2\nend_header\n"
          <<"-0.4 0 0 -1 -1 -1 1 0 0 0 1.5 0 3 0.7 0.8 1\n"
          <<"0.4 0 0 -1 -1 -1 1 0 0 0 1.2 3 3 1 0.7 0.5\n";
        p.close();
        auto imported=dve::import_gabor_ply(ply);require(static_cast<bool>(imported),"PLY import failed");require(imported.asset->primitives.size()==2,"wrong primitive count");require(imported.asset->primitives[0].scale.x>0,"log scale was not decoded");
        const float center=dve::evaluate_gabor_density(*imported.asset,{-.4F,0,0});require(center>0.1F,"density evaluation failed");
        const auto low=dve::gabor_lod_weight(imported.asset->primitives[1],1.0F);const auto high=dve::gabor_lod_weight(imported.asset->primitives[1],1000.0F);require(high>=low,"LOD weight is not monotonic");
        const auto assetPath=root/"test.dgabor";std::string error;require(dve::write_dgabor(assetPath,*imported.asset,&error),error);auto loaded=dve::read_dgabor(assetPath);require(static_cast<bool>(loaded),".dgabor read failed");require(loaded.asset->contentHash==imported.asset->contentHash,"hash changed on round trip");

        const auto trailingPath=root/"trailing.dgabor";std::filesystem::copy_file(assetPath,trailingPath,std::filesystem::copy_options::overwrite_existing);{std::ofstream stream(trailingPath,std::ios::binary|std::ios::app);stream.put('X');}require(!dve::read_dgabor(trailingPath),"trailing .dgabor bytes were accepted");
        const auto truncatedPath=root/"truncated.dgabor";std::filesystem::copy_file(assetPath,truncatedPath,std::filesystem::copy_options::overwrite_existing);std::filesystem::resize_file(truncatedPath,std::filesystem::file_size(truncatedPath)-7);require(!dve::read_dgabor(truncatedPath),"truncated .dgabor was accepted");
        const auto corruptPath=root/"corrupt.dgabor";std::filesystem::copy_file(assetPath,corruptPath,std::filesystem::copy_options::overwrite_existing);{std::fstream stream(corruptPath,std::ios::binary|std::ios::in|std::ios::out);stream.seekg(-1,std::ios::end);char byte{};stream.get(byte);stream.seekp(-1,std::ios::end);stream.put(static_cast<char>(byte^0x01));}require(!dve::read_dgabor(corruptPath),"content-hash corruption was accepted");

        const auto preview=root/"preview.ppm";dve::GaborPreviewOptions po;po.width=64;po.height=48;po.raySteps=48;require(dve::render_gabor_preview_ppm(preview,*loaded.asset,po,&error),error);require(std::filesystem::file_size(preview)>64,"preview was empty");
        auto plan=dve::plan_gabor_volume_frame(*loaded.asset,{});require(plan.enabled&&plan.raySteps==96,"default frame plan failed");
        dve::GaborVolumeRenderSettings disabled;disabled.enabled=false;require(!dve::plan_gabor_volume_frame(*loaded.asset,disabled).enabled,"disabled plan submitted work");
        std::cout<<"DVE Gabor volume tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<"DVE Gabor volume tests failed: "<<e.what()<<'\n';return 1;}
}
