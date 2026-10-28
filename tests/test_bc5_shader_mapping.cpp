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
double angle_degrees(const std::array<float,3>& packed, double nx, double ny, double nz) {
    const double x=packed[0]*2.0-1.0,y=packed[1]*2.0-1.0,z=packed[2]*2.0-1.0;
    const double length=std::sqrt(x*x+y*y+z*z);
    check(std::abs(length-1.0)<1.0e-5,"decoded BC5 normal is not unit length");
    const double dot=(x*nx+y*ny+z*nz)/length;
    return std::acos(std::clamp(dot,-1.0,1.0))*180.0/3.14159265358979323846;
}
void numerical_z_contract() {
    // Encode known positive-Z tangent normals through the real T3 codec (constant 4x4 mips have
    // exact BC4 endpoints), decode the actual BC5 bytes, rebuild Z and compare to the source normal.
    struct Case {double x,y;};
    const std::array<Case,8> cases{{{0,0},{0.5,0},{-0.5,0},{0,0.8},{0.6,-0.6},{0.8,0.5},{-0.92,0.3},{0.99,0}}};
    for(const auto& c:cases) {
        const double z=std::sqrt(1.0-c.x*c.x-c.y*c.y);
        PolygonTextureMip mip{};mip.width=4;mip.height=4;mip.rgba8.resize(64U);
        for(std::size_t i=0;i<16;++i) {
            mip.rgba8[i*4]=static_cast<std::uint8_t>(std::lround((c.x*0.5+0.5)*255.0));
            mip.rgba8[i*4+1]=static_cast<std::uint8_t>(std::lround((c.y*0.5+0.5)*255.0));
            mip.rgba8[i*4+2]=static_cast<std::uint8_t>(std::lround((z*0.5+0.5)*255.0));
            mip.rgba8[i*4+3]=255U;
        }
        PolygonTextureMipChain chain;chain.levels.push_back(mip);
        std::vector<std::vector<std::byte>> blocks;
        std::string error;
        check(encode_bc5_normal_mips(chain,blocks,&error),"codec oracle failed");
        check(blocks.size()==1U && blocks[0].size()==16U,"codec output size wrong");
        const auto rg=decode_bc5_rg_block(std::span<const std::byte,16>(blocks[0].data(),16U));
        for(const auto& pix:rg) {
            const auto n=decode_bc5_normal_sample(static_cast<float>(pix[0])/255.0F,
                                                  static_cast<float>(pix[1])/255.0F);
            check(n[2]>=0.5F,"BC5 reconstruction must stay in the +Z hemisphere");
            check(angle_degrees(n,c.x,c.y,z)<1.0,"BC5 round-trip normal deviates from the source");
        }
    }
    // Flat texel (128,128) must not tilt the surface noticeably; z is rebuilt, never read.
    const auto flat=decode_bc5_normal_sample(128.0F/255.0F,128.0F/255.0F);
    check(std::abs(flat[0]-0.5F)<0.01F && std::abs(flat[1]-0.5F)<0.01F && flat[2]>0.99F,"flat BC5 texel");
    // XY outside the unit disk (invalid after interpolation/quantization): Z clamps to 0, and the
    // result is renormalized instead of producing NaN or a non-unit vector.
    const auto outside=decode_bc5_normal_sample(1.0F,1.0F);
    for(float v:outside) check(std::isfinite(v) && v>=0.0F && v<=1.0F,"clamped normal not finite/saturated");
    check(std::abs(outside[2]-0.5F)<1.0e-6F,"outside-disk sample must have z==0");
    check(std::abs((outside[0]*2.0F-1.0F)-(outside[1]*2.0F-1.0F))<1.0e-6F,"outside-disk direction lost");
    check(angle_degrees(outside,std::sqrt(0.5),std::sqrt(0.5),0.0)<1.0e-3,"outside-disk sample not unit length");
    // Boundary sample exactly on the unit circle keeps z==0 without a sqrt of a negative number.
    const auto edge=decode_bc5_normal_sample(1.0F,0.5F);
    for(float v:edge) check(std::isfinite(v),"unit-circle edge produced non-finite output");
}
}
int main(){try{
    mapping_flag_requires_bound_bc5();numerical_z_contract();
    std::cout<<"T4 BC5 mapping contract: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
