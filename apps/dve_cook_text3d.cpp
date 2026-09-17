#include "dve/text3d.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc,char**argv){
    std::filesystem::path font,output,preview;
    std::string text;
    dve::Text3DCookOptions options;
    for(int i=1;i<argc;++i){const std::string arg=argv[i];
        auto value=[&](){if(i+1>=argc)throw std::runtime_error("missing value for "+arg);return std::string(argv[++i]);};
        if(arg=="--font")font=value();else if(arg=="--text")text=value();else if(arg=="--output")output=value();else if(arg=="--preview")preview=value();
        else if(arg=="--size")options.style.emSizeMeters=std::stof(value());else if(arg=="--depth")options.style.extrusionDepthMeters=std::stof(value());
        else if(arg=="--spacing")options.style.letterSpacingEm=std::stof(value());
        else if(arg=="--face-material")options.style.faceMaterialId=static_cast<std::uint32_t>(std::stoul(value()));
        else if(arg=="--side-material")options.style.sideMaterialId=static_cast<std::uint32_t>(std::stoul(value()));
        else if(arg=="--bands"){const auto v=static_cast<std::uint16_t>(std::stoul(value()));options.style.horizontalBands=v;options.style.verticalBands=v;}
        else if(arg=="--center")options.style.alignment=dve::Text3DHorizontalAlignment::Center;
        else if(arg=="--right")options.style.alignment=dve::Text3DHorizontalAlignment::Right;
        else if(arg=="--even-odd")options.style.fillRule=dve::Text3DFillRule::EvenOdd;
        else if(arg=="--help"){std::cout<<"dve_cook_text3d --font FONT.ttf --text TEXT --output asset.dtext [--preview preview.ppm] [--size M] [--depth M] [--spacing EM] [--face-material ID] [--side-material ID] [--bands N] [--center|--right] [--even-odd]\n";return 0;}
        else throw std::runtime_error("unknown argument: "+arg);
    }
    if(font.empty()||output.empty())throw std::runtime_error("--font and --output are required");
    const auto cooked=dve::cook_text3d(font,text,options);
    if(!cooked){std::cerr<<"3D text cook failed: "<<cooked.error<<'\n';return 1;}
    std::string error;
    if(!dve::write_dtext(output,cooked.asset,&error)){std::cerr<<"3D text write failed: "<<error<<'\n';return 1;}
    if(!preview.empty()&&!dve::write_text3d_preview_ppm(preview,cooked.asset,{},&error)){std::cerr<<"preview failed: "<<error<<'\n';return 1;}
    std::cout<<"wrote "<<output<<" glyphs="<<cooked.asset.glyphInstances.size()<<" curves="<<cooked.asset.atlas.curveTexels.size()/2U<<" side_triangles="<<cooked.asset.sideMesh.indices.size()/3U<<'\n';
    return 0;
}
