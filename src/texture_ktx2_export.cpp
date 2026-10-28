#include "dve/texture_ktx2_export.hpp"
#include <ktx.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

namespace dve {
namespace {
std::uint32_t vk_format(rhi::TextureFormat format) noexcept {
    switch(format) {
    case rhi::TextureFormat::BC1RGBAUnorm: return 133;
    case rhi::TextureFormat::BC1RGBASrgb: return 134;
    case rhi::TextureFormat::BC3RGBAUnorm: return 137;
    case rhi::TextureFormat::BC3RGBASrgb: return 138;
    case rhi::TextureFormat::BC4RUnorm: return 139;
    case rhi::TextureFormat::BC5RGUnorm: return 141;
    case rhi::TextureFormat::BC6HRGBUfloat: return 143;
    case rhi::TextureFormat::BC6HRGBSfloat: return 144;
    case rhi::TextureFormat::BC7RGBAUnorm: return 145;
    case rhi::TextureFormat::BC7RGBASrgb: return 146;
    case rhi::TextureFormat::ETC2RGBUnorm: return 147;
    case rhi::TextureFormat::ETC2RGBSrgb: return 148;
    case rhi::TextureFormat::ETC2RGBAUnorm: return 151;
    case rhi::TextureFormat::ETC2RGBASrgb: return 152;
    case rhi::TextureFormat::EACR11Unorm: return 153;
    case rhi::TextureFormat::EACRG11Unorm: return 155;
    case rhi::TextureFormat::ASTC4x4RGBAUnorm: return 157;
    case rhi::TextureFormat::ASTC4x4RGBASrgb: return 158;
    case rhi::TextureFormat::ASTC6x6RGBAUnorm: return 165;
    case rhi::TextureFormat::ASTC6x6RGBASrgb: return 166;
    case rhi::TextureFormat::ASTC8x8RGBAUnorm: return 171;
    case rhi::TextureFormat::ASTC8x8RGBASrgb: return 172;
    default:return 0;
    }
}
void fail(std::string* error,const std::string& why) {if(error)*error=why;}
void fail_ktx(std::string* error,const char* what,KTX_error_code code) {
    fail(error,std::string(what)+": "+ktxErrorString(code));
}
// A complete chain has floor(log2(max(width,height)))+1 levels. libktx rejects a
// longer chain with KTX_INVALID_OPERATION, so refuse it here with a clear message.
std::uint32_t max_mip_levels(std::uint32_t width,std::uint32_t height) noexcept {
    std::uint32_t levels=1U;
    for(auto extent=std::max(width,height);extent>1U;extent>>=1U)++levels;
    return levels;
}
}
bool encode_native_ktx2(const CompressedTextureAsset& asset,
                        std::vector<std::byte>& output,std::string* error) {
    output.clear();
    const auto vk=vk_format(asset.format);
    if(!vk||asset.mips.empty()||asset.mips.size()>32U) {
        fail(error,"KTX2 requires known native block format and mips");return false;
    }
    const auto w=asset.mips.front().width,h=asset.mips.front().height;
    if(!w||!h||w>16384U||h>16384U) {
        fail(error,"KTX2 invalid dimensions");return false;
    }
    if(asset.mips.size()>max_mip_levels(w,h)) {
        fail(error,"KTX2 mip count "+std::to_string(asset.mips.size())+" exceeds the "+
                   std::to_string(max_mip_levels(w,h))+" levels of a "+std::to_string(w)+"x"+
                   std::to_string(h)+" texture");
        return false;
    }
    std::uint32_t mw=w,mh=h;
    std::size_t total=0;
    for(const auto& mip:asset.mips) {
        const auto bytes=rhi::texture_row_bytes(asset.format,mw)*
                         rhi::texture_rows(asset.format,mh);
        if(mip.width!=mw||mip.height!=mh||mip.blocks.size()!=bytes ||
           bytes>512U*1024U*1024U-total) {
            fail(error,"KTX2 malformed or oversized mip chain");return false;
        }
        total+=bytes;
        mw=std::max(1U,mw/2U);mh=std::max(1U,mh/2U);
    }
    ktxTextureCreateInfo info{};
    info.vkFormat=vk;
    info.baseWidth=w;info.baseHeight=h;info.baseDepth=1;
    info.numDimensions=2;info.numLevels=static_cast<ktx_uint32_t>(asset.mips.size());
    info.numLayers=1;info.numFaces=1;
    info.isArray=KTX_FALSE;info.generateMipmaps=KTX_FALSE;
    ktxTexture2* raw=nullptr;
    const auto created=ktxTexture2_Create(&info,KTX_TEXTURE_CREATE_ALLOC_STORAGE,&raw);
    if(created!=KTX_SUCCESS||!raw) {
        fail_ktx(error,"libktx texture allocation failed",created);return false;
    }
    const std::unique_ptr<ktxTexture2,void(*)(ktxTexture2*)> texture(
        raw,[](ktxTexture2* ptr){ktxTexture_Destroy(ktxTexture(ptr));});
    for(std::size_t i=0;i<asset.mips.size();++i) {
        const auto& mip=asset.mips[i];
        const auto result=ktxTexture_SetImageFromMemory(
            ktxTexture(raw),static_cast<ktx_uint32_t>(i),0,0,
            reinterpret_cast<const ktx_uint8_t*>(mip.blocks.data()),mip.blocks.size());
        if(result!=KTX_SUCCESS) {
            fail_ktx(error,"libktx refused native mip payload",result);return false;
        }
    }
    ktx_uint8_t* bytes=nullptr;
    ktx_size_t size=0;
    const auto result=ktxTexture2_WriteToMemory(raw,&bytes,&size);
    if(result!=KTX_SUCCESS||!bytes||size>512U*1024U*1024U) {
        std::free(bytes);
        if(result!=KTX_SUCCESS) fail_ktx(error,"libktx serialization failed",result);
        else fail(error,"libktx serialization exceeded budget");
        return false;
    }
    output.assign(reinterpret_cast<std::byte*>(bytes),
                  reinterpret_cast<std::byte*>(bytes)+size);
    std::free(bytes);
    return true;
}
} // namespace dve
