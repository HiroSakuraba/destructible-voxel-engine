#include <filesystem>
#include <iostream>
#include <string>

#include "dve/gabor_volume.hpp"

int main(int argc,char** argv){
    std::filesystem::path input,output,preview;float opacityScale=1.0F,density=1.0F,positionScale=1.0F;bool pyramid=false;std::uint32_t previewWidth=160U,previewHeight=120U,previewSteps=48U;
    for(int i=1;i<argc;++i){const std::string a=argv[i];if(a=="--input"&&i+1<argc)input=argv[++i];else if(a=="--output"&&i+1<argc)output=argv[++i];else if(a=="--preview"&&i+1<argc)preview=argv[++i];else if(a=="--opacity-scale"&&i+1<argc)opacityScale=std::stof(argv[++i]);else if(a=="--density"&&i+1<argc)density=std::stof(argv[++i]);else if(a=="--position-scale"&&i+1<argc)positionScale=std::stof(argv[++i]);else if(a=="--pyramid")pyramid=true;else if(a=="--preview-width"&&i+1<argc)previewWidth=static_cast<std::uint32_t>(std::stoul(argv[++i]));else if(a=="--preview-height"&&i+1<argc)previewHeight=static_cast<std::uint32_t>(std::stoul(argv[++i]));else if(a=="--preview-steps"&&i+1<argc)previewSteps=static_cast<std::uint32_t>(std::stoul(argv[++i]));else if(a=="--help"){std::cout<<"dve_cook_gabor --input FILE|DIR --output FILE.dgabor [--pyramid] [--preview FILE.ppm] [--density N] [--opacity-scale N] [--position-scale N] [--preview-width N --preview-height N --preview-steps N]\n";return 0;}}
    if(input.empty()||output.empty()){std::cerr<<"input and output are required\n";return 2;}
    dve::GaborPlyImportOptions options;options.opacityScale=opacityScale;options.densityNormalization=density;options.positionScale=positionScale;
    auto imported=pyramid?dve::import_gabor_pyramid(input,options):dve::import_gabor_ply(input,options);if(!imported){std::cerr<<imported.error<<'\n';return 1;}
    std::string error;if(!dve::write_dgabor(output,*imported.asset,&error)){std::cerr<<error<<'\n';return 1;}dve::GaborPreviewOptions previewOptions;previewOptions.width=previewWidth;previewOptions.height=previewHeight;previewOptions.raySteps=previewSteps;if(!preview.empty()&&!dve::render_gabor_preview_ppm(preview,*imported.asset,previewOptions,&error)){std::cerr<<error<<'\n';return 1;}
    const auto plan=dve::plan_gabor_volume_frame(*imported.asset,{});std::cout<<"primitives="<<imported.asset->primitives.size()<<" submitted="<<plan.submittedPrimitives<<" hash="<<imported.asset->contentHash<<'\n';return 0;
}
