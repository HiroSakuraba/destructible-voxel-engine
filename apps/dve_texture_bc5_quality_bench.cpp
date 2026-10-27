// T3 normal-map quality harness; accepts real DVE RGBA8 bake buffers without PNG dependencies.
// Usage: dve_texture_bc5_quality_bench [WIDTH HEIGHT raw_rgba8_file]
// The default corpus is synthetic and is labeled as such; no game-frame speedup is implied.
#include "dve/render/normal_bc5_codec.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
dve::PolygonImage synthetic(std::uint32_t w,std::uint32_t h,unsigned mode) {
    dve::PolygonImage a;
    a.name=mode==0U?"flat":mode==1U?"wavy_relief":"woven_fabric";
    a.width=w;a.height=h;a.mimeType="image/raw";
    a.rgba8.resize(static_cast<std::size_t>(w)*h*4U);
    for(std::uint32_t y=0;y<h;++y)for(std::uint32_t x=0;x<w;++x) {
        const double fx=static_cast<double>(x),fy=static_cast<double>(y);
        const double nx=mode==0U?0.0:(mode==1U?0.4*std::sin(fx*0.10):0.30*std::sin(fx*0.41));
        const double ny=mode==0U?0.0:(mode==1U?0.4*std::cos(fy*0.12):0.30*std::cos(fy*0.43));
        const double z=std::sqrt(std::max(0.0,1.0-nx*nx-ny*ny));
        const auto b=(static_cast<std::size_t>(y)*w+x)*4U;
        a.rgba8[b]=static_cast<std::uint8_t>(std::clamp((nx*0.5+0.5)*255.0+0.5,0.0,255.0));
        a.rgba8[b+1U]=static_cast<std::uint8_t>(std::clamp((ny*0.5+0.5)*255.0+0.5,0.0,255.0));
        a.rgba8[b+2U]=static_cast<std::uint8_t>(std::clamp((z*0.5+0.5)*255.0+0.5,0.0,255.0));
        a.rgba8[b+3U]=255U;
    }
    return a;
}
void measure(const dve::PolygonImage& image,bool real) {
    const auto chain=dve::render::generate_semantic_texture_mips(
        image,dve::render::TextureSemantic::Normal);
    std::vector<std::vector<std::byte>> blocks;
    std::string error;
    const auto start=std::chrono::steady_clock::now();
    if(!dve::render::encode_bc5_normal_mips(chain,blocks,&error))
        throw std::runtime_error("BC5 encode failed: "+error);
    const double ms=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    std::size_t packed=0U,raw=0U;
    for(std::size_t i=0;i<chain.levels.size();++i){packed+=blocks[i].size();raw+=chain.levels[i].rgba8.size();}
    const auto& level=chain.levels.front();
    const auto degrees=dve::render::bc5_mean_normal_angle_degrees(level,blocks.front());
    std::cout<<"{\"source\":\""<<(real?"provided_dve_rgba8":"synthetic_" + image.name)
             <<"\",\"width\":"<<image.width<<",\"height\":"<<image.height
             <<",\"mips\":"<<chain.levels.size()<<",\"rgba_bytes\":"<<raw
             <<",\"bc5_bytes\":"<<packed<<",\"mean_normal_angle_deg\":"<<degrees
             <<",\"encode_ms\":"<<ms<<"}\n";
}
}
int main(int argc,char** argv) {
    try {
        if(argc==4) {
            const auto w=static_cast<std::uint32_t>(std::stoul(argv[1]));
            const auto h=static_cast<std::uint32_t>(std::stoul(argv[2]));
            if(!w||!h||w>8192U||h>8192U)throw std::runtime_error("invalid input size");
            dve::PolygonImage image;image.name="user";image.width=w;image.height=h;
            image.mimeType="image/raw";
            const auto size=static_cast<std::size_t>(w)*h*4U;
            image.rgba8.resize(size);
            std::ifstream in(argv[3],std::ios::binary);
            if(!in||!in.read(reinterpret_cast<char*>(image.rgba8.data()),
                              static_cast<std::streamsize>(size))||in.peek()!=std::char_traits<char>::eof())
                throw std::runtime_error("expected exact WIDTH*HEIGHT*4 RGBA8 file");
            measure(image,true);
        } else if(argc==1) {
            for(unsigned mode=0;mode<3U;++mode)measure(synthetic(512U,512U,mode),false);
        } else throw std::runtime_error("usage: dve_texture_bc5_quality_bench [width height file.rgba8]");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
