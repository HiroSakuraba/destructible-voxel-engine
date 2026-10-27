// T5 quality validation uses images produced by DVE's real material baker,
// not a hand-built mip texture or canned analytic normal-map fixture.
#include "dve/editor_polygon.hpp"
#include "dve/render/material_texture_bake.hpp"
#include "dve/render/normal_bc5_codec.hpp"
#include "dve/render/normal_texture_binding.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/rhi/null_device.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::render;
namespace {
void require(bool ok,const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}
CookedPolygonAsset input_mesh(unsigned pattern) {
    auto asset = editor::make_editor_polygon_plane(1,1);
    asset.contentHash=0U;
    for(auto& vertex:asset.vertices) vertex.texcoord1=vertex.texcoord;
    PolygonImage normal;
    normal.name="DVE authored normal";normal.mimeType="image/raw";
    normal.width=32U;normal.height=32U;normal.rgba8.resize(32U*32U*4U);
    for(unsigned y=0;y<32U;++y) for(unsigned x=0;x<32U;++x) {
        const double fx=static_cast<double>(x),fy=static_cast<double>(y);
        const double nx=pattern==0U?0.0:pattern==1U?
            0.24*std::sin(fx*0.23):0.32*std::sin(fx*0.53+fy*0.13);
        const double ny=pattern==0U?0.0:pattern==1U?
            0.23*std::cos(fy*0.24):0.28*std::cos(fx*0.09+fy*0.49);
        const double nz=std::sqrt(std::max(0.0,1.0-nx*nx-ny*ny));
        const auto at=(y*32U+x)*4U;
        auto byte=[](double n) {return static_cast<std::uint8_t>(
            std::clamp((n*0.5+0.5)*255.0+0.5,0.0,255.0));};
        normal.rgba8[at]=byte(nx);normal.rgba8[at+1U]=byte(ny);
        normal.rgba8[at+2U]=byte(nz);normal.rgba8[at+3U]=255U;
    }
    asset.images.push_back(std::move(normal));
    asset.samplers.push_back({});
    asset.textures.push_back({"input-normal",0U,0U});
    asset.materialBindings[0].normal={0U,0U,PolygonTextureColorSpace::Linear};
    return asset;
}
PolygonImage decode_bc5_to_rgba(const PolygonImage& original,
                               std::span<const std::byte> blocks) {
    auto reconstructed=original;
    const auto tilesX=(original.width+3U)/4U;
    for(std::uint32_t y=0;y<original.height;++y)
        for(std::uint32_t x=0;x<original.width;++x) {
            const auto tile=(static_cast<std::size_t>(y/4U)*tilesX+x/4U)*16U;
            const auto texels=decode_bc5_rg_block(
                std::span<const std::byte,16>(blocks.data()+tile,16U));
            const auto p=texels[(y%4U)*4U+(x%4U)];
            const auto normal=decode_bc5_normal_sample(
                static_cast<float>(p[0])/255.0F,static_cast<float>(p[1])/255.0F);
            const auto offset=(static_cast<std::size_t>(y)*original.width+x)*4U;
            for(std::size_t k=0;k<3U;++k)
                reconstructed.rgba8[offset+k]=static_cast<std::uint8_t>(
                    std::clamp(normal[k]*255.0F+0.5F,0.0F,255.0F));
        }
    return reconstructed;
}
void run_one(unsigned pattern) {
    auto mesh=input_mesh(pattern);
    MaterialTextureBakeSettings settings{};
    settings.resolution=64U;
    settings.samplesPerAxis=2U;
    settings.paddingPixels=4U;
    settings.maps=static_cast<std::uint32_t>(MaterialBakeMap::Normal);
    const auto baked=bake_material_textures(mesh,settings);
    require(baked.success,"DVE material baking failed: "+baked.error);
    require(baked.coveredPixels>0U && baked.surfaceSamples>0U,
            "DVE baker returned no rendered normal samples");
    const auto& image=baked.images.at(MaterialBakeMap::Normal);
    require(image.width==settings.resolution && image.height==settings.resolution,
            "unexpected baked normal dimensions");
    // The bake must carry the authored pattern: a baker that ignored the input normal map would
    // return a flat map, and BC5 would then trivially "pass" the error bounds below.
    double maxTilt=0.0,meanSlope=0.0;
    for(std::size_t p=0;p<image.rgba8.size()/4U;++p) {
        const double nx=static_cast<double>(image.rgba8[4U*p])/127.5-1.0;
        const double ny=static_cast<double>(image.rgba8[4U*p+1U])/127.5-1.0;
        const double nz=static_cast<double>(image.rgba8[4U*p+2U])/127.5-1.0;
        maxTilt=std::max(maxTilt,std::acos(std::clamp(nz/std::sqrt(nx*nx+ny*ny+nz*nz),-1.0,1.0))
                                  *180.0/3.14159265358979323846);
        meanSlope+=(std::abs(nx)+std::abs(ny))/static_cast<double>(image.rgba8.size()/4U);
    }
    if(pattern==0U)
        require(maxTilt<1.0,"flat authored normal did not bake to a flat map");
    else
        require(maxTilt>10.0&&meanSlope>0.1,
                "baked normal lost the authored ripple/woven pattern (baker ignored the normal map?)");
    const auto mips=generate_semantic_texture_mips(image,TextureSemantic::Normal);
    std::vector<std::vector<std::byte>> blocks,repeat;
    std::string error;
    require(encode_bc5_normal_mips(mips,blocks,&error),
            "BC5 rejected an authentic DVE bake: "+error);
    require(encode_bc5_normal_mips(mips,repeat,&error)&&blocks==repeat,
            "BC5 encoding of DVE bake is not deterministic");
    std::size_t rgbaBytes=0U,bc5Bytes=0U;
    for(std::size_t i=0;i<mips.levels.size();++i) {
        rgbaBytes+=mips.levels[i].rgba8.size();
        bc5Bytes+=blocks[i].size();
        const auto angle=bc5_mean_normal_angle_degrees(mips.levels[i],blocks[i]);
        require(angle>=0.0 && angle<3.0,"DVE baked normal mean angular error exceeds 3 degrees");
    }
    require(bc5Bytes*2U<=rgbaBytes+32U,"BC5 bake is not substantially smaller than RGBA8");
    const auto reconstructed=decode_bc5_to_rgba(image,blocks.front());
    // CPU differential oracle: top-level RGBA pixels after BC5 decode are
    // within a bounded angular error of the actual DVE-baked RGBA normals.
    double worst=0.0;
    for(std::size_t p=0;p<image.rgba8.size()/4U;++p) {
        auto unpack=[](const PolygonImage& img,std::size_t i) {
            std::array<double,3> n{};
            for(std::size_t c=0;c<3U;++c)
                n[c]=static_cast<double>(img.rgba8[4U*i+c])/127.5-1.0;
            const double norm=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            for(double& v:n)v/=std::max(norm,1.0e-9);
            return n;
        };
        const auto a=unpack(image,p),b=unpack(reconstructed,p);
        const double angle=std::acos(std::clamp(a[0]*b[0]+a[1]*b[1]+a[2]*b[2],-1.0,1.0))
            *180.0/3.14159265358979323846;
        worst=std::max(worst,angle);
    }
    require(worst<4.0,"maximum baked texel normal deviation exceeds 4 degrees");
    // These images are from the real DVE bake engine. Null RHI verifies the
    // exact encoded GPU byte payload, while HLSL math is checked in T4 tests.
    rhi::NullDevice gpu;
    TextureResidencyManager manager(gpu,65536U,{},make_bc5_normal_encoder());
    require(manager.enqueue({100U+pattern,image,TextureSemantic::Normal,
        TextureCompressionPreference::PreferBc5ForNormals,true},&error),
        "cannot enqueue actual baked DVE normal: "+error);
    require(manager.pump(65536U,&error)>0U,"cannot upload baked BC5 texture");
    const auto resident=manager.find(100U+pattern);
    require(resident && resident->format==rhi::TextureFormat::BC5RGUnorm,
            "authentic DVE baked texture did not select native BC5");
    require(resident->residentBytes==bc5Bytes,
            "authentic DVE baked BC5 resident bytes mismatch");
    manager.clear();
    require(gpu.statistics().texturesCreated==gpu.statistics().texturesDestroyed,
            "authentic baked BC5 test leaked textures");
    static constexpr const char* names[]={"flat","ripple","woven"};
    std::cout<<"DVE baked normal "<<names[pattern]
             <<" rgba_bytes="<<rgbaBytes<<" bc5_bytes="<<bc5Bytes
             <<" first_mip_mean_angle_deg="
             <<bc5_mean_normal_angle_degrees(mips.levels.front(),blocks.front())
             <<" worst_texel_angle_deg="<<worst<<"\n";
}
}
int main(){try{
    for(unsigned pattern=0U;pattern<3U;++pattern)run_one(pattern);
    std::cout<<"T5 actual DVE material bake BC5 quality: PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
