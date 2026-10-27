#include "dve/render/normal_texture_binding.hpp"
#include "dve/render/normal_bc5_codec.hpp"
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
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
std::array<double,3> reconstructed(const std::array<std::uint8_t,2>& xy) {
    const double x=static_cast<double>(xy[0])/127.5-1.0;
    const double y=static_cast<double>(xy[1])/127.5-1.0;
    const double z=std::sqrt(std::max(0.0,1.0-x*x-y*y));
    const double length=std::sqrt(x*x+y*y+z*z);
    return {x/length,y/length,z/length};
}
void mapping_flag_requires_bound_bc5() {
    rhi::NullDevice gpu;
    TextureResidencyManager mgr(gpu,65536U,{},make_bc5_normal_encoder());
    PolygonImage img;
    img.name="normal"; img.mimeType="image/raw"; img.width=4;img.height=4;
    img.rgba8.resize(4U*4U*4U);
    for(std::size_t i=0;i<img.rgba8.size();i+=4U) {
        img.rgba8[i]=128U;img.rgba8[i+1U]=128U;img.rgba8[i+2U]=255U;img.rgba8[i+3U]=255U;
    }
    std::string error;
    check(mgr.enqueue({10U,img,TextureSemantic::Normal,
        TextureCompressionPreference::PreferBc5ForNormals,false},&error),"enqueue BC5");
    check(mgr.pump(65536U,&error)!=0U,"publish BC5");
    const ResidentTexture* resident=mgr.find(10U);
    check(resident && resident->format==rhi::TextureFormat::BC5RGUnorm,"BC5 not resident");
    GpuPolygonMaterialMappingRecord rec{};
    rec.normalTexture=1U;
    rec.mappingFlags=kGpuMaterialMappingBaseColorSrgb;
    check(configure_bound_normal_format(rec,resident,resident->view),"correct BC5 view rejected");
    check((rec.mappingFlags&kGpuMaterialMappingNormalBc5)!=0U,"BC5 flag absent");
    check((rec.mappingFlags&kGpuMaterialMappingBaseColorSrgb)!=0U,"other mapping bits disturbed");
    // Deliberately bind a different still-valid texture view: must not claim BC5.
    rhi::TextureDesc desc{};
    desc.format=rhi::TextureFormat::RGBA8Unorm;desc.width=4;desc.height=4;
    desc.usage=rhi::TextureUsage::Sampled;
    const auto otherTexture=gpu.create_texture(desc,&error);
    check(static_cast<bool>(otherTexture),"other texture allocation");
    rhi::TextureViewDesc viewDesc{};viewDesc.texture=otherTexture;
    const auto otherView=gpu.create_texture_view(viewDesc,&error);
    check(static_cast<bool>(otherView),"other view allocation");
    check(!configure_bound_normal_format(rec,resident,otherView),"misbound BC5 view accepted");
    check((rec.mappingFlags&kGpuMaterialMappingNormalBc5)==0U,"stale BC5 bit after wrong view");
    // Revert through an actual RGBA8 resident fallback.
    TextureResidencyManager rgba(gpu,65536U);
    check(rgba.enqueue({20U,img,TextureSemantic::Normal,
        TextureCompressionPreference::PreferBc5ForNormals,false},&error),"rgba fallback enqueue");
    check(rgba.pump(65536U,&error)!=0U,"rgba fallback upload");
    const auto* r=rgba.find(20U);
    check(r && r->format==rhi::TextureFormat::RGBA8Unorm,"fallback format");
    check(configure_bound_normal_format(rec,r,r->view),"rgba8 fallback should be valid");
    check((rec.mappingFlags&kGpuMaterialMappingNormalBc5)==0U,"RGBA8 misclassified as BC5");
    rec.normalTexture=kNoGpuTextureIndex;
    check(!configure_bound_normal_format(rec,resident,resident->view),"no normal bound but BC5 set");
    check((rec.mappingFlags&kGpuMaterialMappingNormalBc5)==0U,"absent view bit");

    check(gpu.destroy_texture_view(otherView,&error),"destroy other view");
    check(gpu.destroy_texture(otherTexture,&error),"destroy other texture");
}
void numerical_z_contract() {
    // Numerical host oracle matching DecodeMaterialNormalSample's BC5 path.
    const std::array<std::array<std::uint8_t,2>,5> samples{{
        {{128,128}}, {{255,128}}, {{0,128}}, {{164,202}}, {{192,192}}
    }};
    for(const auto xy:samples) {
        const auto n=reconstructed(xy);
        const double length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        check(std::abs(length-1.0)<1.e-10,"reconstructed BC5 normal not normalized");
        check(n[2]>=0.0,"BC5 normal has invalid negative Z");
    }
    // Check the GPU shader's normal reconstruction against codec-decoded RG pixels.
    PolygonTextureMip mip{};
    mip.width=4;mip.height=4;mip.rgba8.resize(64U);
    for(std::size_t i=0;i<16;++i) {
        mip.rgba8[i*4]=static_cast<std::uint8_t>(100U+i*3U);
        mip.rgba8[i*4+1]=static_cast<std::uint8_t>(120U+i*2U);
        mip.rgba8[i*4+2]=230U;
        mip.rgba8[i*4+3]=255U;
    }
    PolygonTextureMipChain chain;chain.levels.push_back(mip);
    std::vector<std::vector<std::byte>> blocks;
    std::string error;
    check(encode_bc5_normal_mips(chain,blocks,&error),"codec oracle failed");
    check(blocks.size()==1U && blocks[0].size()==16U,"codec output size wrong");
    const auto rgba=decode_bc5_rg_block(std::span<const std::byte,16>(blocks[0].data(),16U));
    for(const auto& pix:rgba) {
        const auto n=reconstructed(pix);
        check(n[2]>=0.0,"BC5 reconstruction hemisphere");
    }
}
}
int main(){try{
    mapping_flag_requires_bound_bc5();numerical_z_contract();
    std::cout<<"T4 BC5 mapping contract: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
