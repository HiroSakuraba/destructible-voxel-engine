#include "dve/texture_native_cook.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>

namespace dve {
namespace {
void put(std::vector<std::byte>& out,std::size_t offset,std::uint32_t value) {
    for(std::size_t i=0;i<4U;++i)
        out[offset+i]=static_cast<std::byte>((value>>(8U*i))&255U);
}
std::uint32_t dxgi(rhi::TextureFormat f) noexcept {
    switch(f) {
    case rhi::TextureFormat::BC1RGBAUnorm: return 71U;
    case rhi::TextureFormat::BC1RGBASrgb: return 72U;
    case rhi::TextureFormat::BC3RGBAUnorm: return 77U;
    case rhi::TextureFormat::BC3RGBASrgb: return 78U;
    case rhi::TextureFormat::BC4RUnorm: return 80U;
    case rhi::TextureFormat::BC5RGUnorm: return 83U;
    case rhi::TextureFormat::BC6HRGBUfloat: return 95U;
    case rhi::TextureFormat::BC6HRGBSfloat: return 96U;
    case rhi::TextureFormat::BC7RGBAUnorm: return 98U;
    case rhi::TextureFormat::BC7RGBASrgb: return 99U;
    default: return 0U;
    }
}
void fail(std::string* err,const char* message) {if(err)*err=message;}
} // namespace

bool encode_native_bc_dds(const CompressedTextureAsset& texture,
                          std::vector<std::byte>& dds,std::string* error) {
    dds.clear();
    const auto format=dxgi(texture.format);
    if(!format||texture.mips.empty()||texture.mips.size()>32U) {
        fail(error,"DDS export requires supported BC formats and a bounded mip chain");
        return false;
    }
    const auto width=texture.mips.front().width,height=texture.mips.front().height;
    if(!width||!height||width>16384U||height>16384U) {
        fail(error,"invalid DDS dimensions");return false;
    }
    std::size_t total=148U;
    std::uint32_t mw=width,mh=height;
    for(const auto& mip:texture.mips) {
        const auto expected=rhi::texture_row_bytes(texture.format,mw)*
                            rhi::texture_rows(texture.format,mh);
        if(mip.width!=mw||mip.height!=mh||mip.blocks.size()!=expected ||
           total>512U*1024U*1024U-expected) {
            fail(error,"DDS mip dimensions, byte size, or total budget invalid");
            return false;
        }
        total+=expected;
        mw=std::max(1U,mw/2U);mh=std::max(1U,mh/2U);
    }
    dds.resize(total);
    put(dds,0U,0x20534444U); // DDS
    put(dds,4U,124U); // DDS_HEADER size
    // DDSD_CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE. Block-compressed payloads store the
    // top-level byte size in dwPitchOrLinearSize, so DDSD_LINEARSIZE (not DDSD_PITCH) applies.
    put(dds,8U,0x1U|0x2U|0x4U|0x1000U|0x20000U|0x80000U);
    put(dds,12U,height);put(dds,16U,width);
    put(dds,20U,static_cast<std::uint32_t>(texture.mips.front().blocks.size()));
    put(dds,28U,static_cast<std::uint32_t>(texture.mips.size()));
    put(dds,76U,32U); // DDS_PIXELFORMAT size
    put(dds,80U,0x4U); // DDS_FOURCC
    put(dds,84U,0x30315844U); // 'DX10'
    // DDSCAPS_TEXTURE always; COMPLEX|MIPMAP only when a mip chain is present.
    put(dds,108U,0x1000U|(texture.mips.size()>1U?(0x8U|0x400000U):0U));
    put(dds,128U,format);
    put(dds,132U,3U); // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    put(dds,136U,0U); // misc flag
    put(dds,140U,1U); // one array element
    put(dds,144U,0U);
    std::size_t cursor=148U;
    for(const auto& mip:texture.mips) {
        std::copy(mip.blocks.begin(),mip.blocks.end(),
                  dds.begin()+static_cast<std::ptrdiff_t>(cursor));
        cursor+=mip.blocks.size();
    }
    return true;
}
} // namespace dve
