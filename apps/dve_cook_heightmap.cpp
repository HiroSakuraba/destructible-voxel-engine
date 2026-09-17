#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/dvox.hpp"
#include "dve/heightmap_terrain.hpp"

namespace {

[[nodiscard]] float parse_float(const char* text,const char* option){
    char* end=nullptr;const float value=std::strtof(text,&end);if(!end||*end!='\0')throw std::runtime_error(std::string("invalid value for ")+option);return value;
}
[[nodiscard]] std::uint32_t parse_u32(const char* text,const char* option){
    char* end=nullptr;const unsigned long value=std::strtoul(text,&end,10);if(!end||*end!='\0'||value>0xFFFFFFFFUL)throw std::runtime_error(std::string("invalid value for ")+option);return static_cast<std::uint32_t>(value);
}
void usage(){
    std::cerr<<"Usage: dve_cook_heightmap <input.(png|pgm|r16)> <output-base> [options]\n"
             <<"  --spacing-x <m>       Horizontal sample spacing (default 1)\n"
             <<"  --spacing-z <m>       Vertical-image sample spacing in world Z (default 1)\n"
             <<"  --height-scale <m>    Elevation represented by white (default 100)\n"
             <<"  --height-offset <m>   Elevation represented by black (default 0)\n"
             <<"  --base-height <m>     Solid voxel terrain base relative to origin (default -10)\n"
             <<"  --voxel               Also cook a solid .dvox terrain\n"
             <<"  --voxel-size <m>      Voxel size for --voxel (default 0.25)\n"
             <<"  --no-polygon          Do not write .dmesh\n"
             <<"  --no-nav              Do not write .dnav\n"
             <<"  --slope <degrees>     Maximum navmesh walkable slope (default 45)\n"
             <<"  --agent-radius <m>    Navmesh portal erosion radius (default 0.3)\n"
             <<"  --flip-y              Flip image rows\n"
             <<"  --invert              Invert height samples\n"
             <<"  --raw-width <pixels>  Required for RAW16\n"
             <<"  --raw-height <pixels> Required for RAW16\n"
             <<"  --raw-be               RAW16 is big endian (default little endian)\n";
}

} // namespace

int main(int argc,char** argv){
    try{
        if(argc<3){usage();return 2;}
        const std::filesystem::path input=argv[1],base=argv[2];
        dve::HeightmapImportSettings importSettings;dve::HeightmapTerrainSettings terrain;
        for(int i=3;i<argc;++i){const std::string option=argv[i];auto value=[&](){if(i+1>=argc)throw std::runtime_error("missing value after "+option);return argv[++i];};
            if(option=="--spacing-x")terrain.spacingXMeters=parse_float(value(),"--spacing-x");
            else if(option=="--spacing-z")terrain.spacingZMeters=parse_float(value(),"--spacing-z");
            else if(option=="--height-scale")terrain.heightScaleMeters=parse_float(value(),"--height-scale");
            else if(option=="--height-offset")terrain.heightOffsetMeters=parse_float(value(),"--height-offset");
            else if(option=="--base-height")terrain.solidBaseHeightMeters=parse_float(value(),"--base-height");
            else if(option=="--voxel-size")terrain.voxelSizeMeters=parse_float(value(),"--voxel-size");
            else if(option=="--slope")terrain.navigation.maximumSlopeDegrees=parse_float(value(),"--slope");
            else if(option=="--agent-radius")terrain.navigation.agentRadiusMeters=parse_float(value(),"--agent-radius");
            else if(option=="--raw-width")importSettings.rawWidth=parse_u32(value(),"--raw-width");
            else if(option=="--raw-height")importSettings.rawHeight=parse_u32(value(),"--raw-height");
            else if(option=="--voxel")terrain.generateVoxelAsset=true;
            else if(option=="--no-polygon")terrain.generatePolygonAsset=false;
            else if(option=="--no-nav")terrain.generateNavigationMesh=false;
            else if(option=="--flip-y")importSettings.flipVertical=true;
            else if(option=="--invert")importSettings.invert=true;
            else if(option=="--raw-be")importSettings.format=dve::HeightmapFormat::Raw16BigEndian;
            else if(option=="--help"||option=="-h"){usage();return 0;}
            else throw std::runtime_error("unknown option: "+option);
        }
        const auto result=dve::cook_heightmap_terrain(input,importSettings,terrain);
        for(const auto& d:result.diagnostics){const char* level=d.severity==dve::HeightmapTerrainDiagnostic::Severity::Error?"error":d.severity==dve::HeightmapTerrainDiagnostic::Severity::Warning?"warning":"info";std::cerr<<level<<" ["<<d.code<<"] "<<d.message<<"\n";}
        if(!result.success())return 1;
        std::string error;
        if(result.polygon){auto path=base;path.replace_extension(".dmesh");if(!dve::write_dmesh(path,*result.polygon,&error))throw std::runtime_error(error);std::cout<<"wrote "<<path<<" ("<<result.polygon->vertices.size()<<" vertices, "<<result.polygon->indices.size()/3U<<" triangles)\n";}
        if(result.voxel){auto path=base;path.replace_extension(".dvox");if(!dve::write_dvox(path,*result.voxel,{},&error))throw std::runtime_error(error);std::cout<<"wrote "<<path<<" ("<<result.voxel->object.occupied_voxel_count()<<" voxels)\n";}
        if(result.navigation){auto path=base;path.replace_extension(".dnav");if(!dve::write_dnav(path,*result.navigation,&error))throw std::runtime_error(error);std::cout<<"wrote "<<path<<" ("<<result.navigation->polygons.size()<<" walkable polygons)\n";}
        return 0;
    }catch(const std::exception& e){std::cerr<<"dve_cook_heightmap: "<<e.what()<<"\n";return 1;}
}
