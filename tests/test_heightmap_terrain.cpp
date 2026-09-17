#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <png.h>

#include "dve/dvox.hpp"
#include "dve/heightmap_terrain.hpp"
#include "dve/navigation_geometry.hpp"

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

void write_png16(const std::filesystem::path& path){
    FILE* f=std::fopen(path.string().c_str(),"wb");require(f!=nullptr,"could not create PNG fixture");
    png_structp png=png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);png_infop info=png_create_info_struct(png);require(png&&info,"could not create PNG writer");
    if(setjmp(png_jmpbuf(png))!=0){png_destroy_write_struct(&png,&info);std::fclose(f);throw std::runtime_error("PNG fixture encode failed");}
    png_init_io(png,f);png_set_IHDR(png,info,3U,3U,16,PNG_COLOR_TYPE_GRAY,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);png_write_info(png,info);
    std::uint16_t values[9]{0U,8192U,16384U,8192U,32768U,49152U,16384U,49152U,65535U};
    std::vector<std::uint8_t> bytes(18U);for(std::size_t i=0;i<9U;++i){bytes[i*2U]=static_cast<std::uint8_t>(values[i]>>8U);bytes[i*2U+1U]=static_cast<std::uint8_t>(values[i]&0xFFU);}png_bytep rows[3]{bytes.data(),bytes.data()+6U,bytes.data()+12U};png_write_image(png,rows);png_write_end(png,nullptr);png_destroy_write_struct(&png,&info);std::fclose(f);
}

void test_pgm_and_terrain_build(const std::filesystem::path& root){
    const auto pgm=root/"terrain.pgm";std::ofstream out(pgm);out<<"P2\n3 3\n65535\n0 8192 16384\n8192 32768 49152\n16384 49152 65535\n";out.close();
    dve::HeightmapTerrainSettings settings;settings.spacingXMeters=2.0F;settings.spacingZMeters=3.0F;settings.heightScaleMeters=10.0F;settings.generateVoxelAsset=true;settings.voxelSizeMeters=1.0F;settings.solidBaseHeightMeters=-1.0F;settings.navigation.agentRadiusMeters=0.05F;settings.navigation.maximumSlopeDegrees=85.0F;
    const auto result=dve::cook_heightmap_terrain(pgm,{},settings);require(result.success(),"PGM terrain cook failed");require(result.heightmap.width==3U&&result.heightmap.height==3U,"PGM dimensions wrong");
    require(result.polygon.has_value(),"polygon terrain missing");require(result.polygon->vertices.size()==9U,"polygon grid vertex count wrong");require(result.polygon->indices.size()==24U,"polygon grid triangle count wrong");
    require(result.voxel.has_value()&&result.voxel->object.occupied_voxel_count()>0U,"voxel terrain missing");require(result.navigation.has_value()&&!result.navigation->polygons.empty(),"terrain navmesh missing");
    const auto path=dve::find_navigation_path(*result.navigation,{0.1F,0.1F,0.1F},{3.9F,9.9F,5.9F});require(static_cast<bool>(path),"terrain navmesh path failed");

    std::string error;
    const auto meshPath=root/"terrain.dmesh",voxelPath=root/"terrain.dvox",navPath=root/"terrain.dnav";
    require(dve::write_dmesh(meshPath,*result.polygon,&error),"terrain dmesh write failed");
    require(static_cast<bool>(dve::read_dmesh(meshPath)),"terrain dmesh read failed");
    require(dve::write_dvox(voxelPath,*result.voxel,{},&error),"terrain dvox write failed");
    require(dve::read_dvox(voxelPath).success,"terrain dvox read failed");
    require(dve::write_dnav(navPath,*result.navigation,&error),"terrain dnav write failed");
    require(dve::read_dnav(navPath,&error).has_value(),"terrain dnav read failed");
}

void test_png16_and_raw(const std::filesystem::path& root){
    const auto png=root/"terrain16.png";write_png16(png);std::string error;const auto image=dve::import_heightmap(png,{},&error);require(image.has_value(),"16-bit PNG import failed");require(image->sourceBitDepth==16U,"16-bit PNG depth was not preserved");require(image->at(2U,2U)>0.999F,"16-bit PNG maximum sample wrong");
    const auto raw=root/"terrain.r16";std::ofstream output(raw,std::ios::binary);const std::uint16_t samples[4]{0U,16384U,32768U,65535U};for(auto value:samples){const char bytes[2]{static_cast<char>(value&0xFFU),static_cast<char>(value>>8U)};output.write(bytes,2);}output.close();
    dve::HeightmapImportSettings rawSettings;rawSettings.rawWidth=2U;rawSettings.rawHeight=2U;const auto rawImage=dve::import_heightmap(raw,rawSettings,&error);require(rawImage.has_value(),"RAW16 import failed");require(rawImage->at(1U,1U)>0.999F,"RAW16 maximum sample wrong");
}
}

int main(){try{const auto root=std::filesystem::temp_directory_path()/"dve_heightmap_terrain_tests";std::error_code ec;std::filesystem::remove_all(root,ec);std::filesystem::create_directories(root);test_pgm_and_terrain_build(root);test_png16_and_raw(root);std::filesystem::remove_all(root,ec);std::cout<<"heightmap terrain tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<"heightmap terrain test failure: "<<e.what()<<"\n";return 1;}}
