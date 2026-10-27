#include "dve/texture_native_cook.hpp"
#include "dve/render/normal_bc5_codec.hpp"
#include "dve/rhi/null_device.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
dve::CompressedTextureAsset asset(dve::rhi::TextureFormat format,
                                    std::uint32_t w,std::uint32_t h,unsigned count) {
    dve::CompressedTextureAsset image;image.name="native";image.format=format;
    for(unsigned i=0;i<count;++i) {
        dve::CompressedTextureMip level;
        level.width=w;level.height=h;
        level.blocks.resize(dve::rhi::texture_row_bytes(format,w)*
                            dve::rhi::texture_rows(format,h));
        for(std::size_t j=0;j<level.blocks.size();++j)
            level.blocks[j]=static_cast<std::byte>((j+i*19U)&255U);
        image.mips.push_back(std::move(level));
        w=std::max(1U,w/2U);h=std::max(1U,h/2U);
    }
    return image;
}
void roundtrip(dve::rhi::TextureFormat format) {
    auto image=asset(format,7U,5U,3U);
    std::vector<std::byte> dds;std::string error;
    check(dve::encode_native_bc_dds(image,dds,&error),"DDS cook failed");
    check(dds.size()==148U+96U,"unexpected DDS file size for 7x5 BC 3 mips");
    dve::CompressedTextureAsset decoded;
    check(dve::parse_compressed_texture(dds,"roundtrip",decoded,&error),"DDS import failed");
    check(decoded.format==image.format && decoded.mips.size()==image.mips.size(),
          "DDS roundtrip format or mip count mismatch");
    for(std::size_t i=0;i<image.mips.size();++i)
        check(decoded.mips[i].blocks==image.mips[i].blocks &&
              decoded.mips[i].width==image.mips[i].width &&
              decoded.mips[i].height==image.mips[i].height,"DDS payload corruption");
    dve::rhi::NullDevice gpu;
    dve::CompressedTextureResource resource;
    check(dve::upload_compressed_texture(gpu,decoded,resource,&error),"DDS Null RHI upload failed");
    dve::destroy_compressed_texture(gpu,resource);

    auto corrupt=image;
    corrupt.mips.back().blocks.pop_back();
    check(!dve::encode_native_bc_dds(corrupt,dds,&error),"DDS accepted malformed mip");
    check(dds.empty(),"DDS failure retained stale serialized bytes");
}
}
int main(){try {
    roundtrip(dve::rhi::TextureFormat::BC5RGUnorm);
    roundtrip(dve::rhi::TextureFormat::BC7RGBAUnorm);
    roundtrip(dve::rhi::TextureFormat::BC7RGBASrgb);
    roundtrip(dve::rhi::TextureFormat::BC6HRGBUfloat);
    auto unsupported=asset(dve::rhi::TextureFormat::ASTC6x6RGBAUnorm,7U,5U,1U);
    std::vector<std::byte> out;std::string err;
    check(!dve::encode_native_bc_dds(unsupported,out,&err),
          "DDS should reject ASTC: use KTX2 exporter");
    std::cout<<"T3 native DDS cook: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
