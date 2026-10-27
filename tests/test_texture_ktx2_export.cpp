// Independent KTX2 interoperability: DVE exports, libktx parses and reads
// the image bytes itself, then DVE's own KTX2 loader also parses them.
#include "dve/texture_ktx2_export.hpp"
#include "dve/compressed_texture.hpp"
#include "dve/rhi/null_device.hpp"
#include <ktx.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
void check(bool pass,const std::string& msg) {if(!pass)throw std::runtime_error(msg);}
struct Case { rhi::TextureFormat format; std::uint32_t vk,w,h;};
void verify(const Case& c) {
    CompressedTextureAsset input;
    input.name="KTX2 independent validation";input.format=c.format;
    auto w=c.w,h=c.h;
    // Complete 4-level chain probes mip ordering and edge blocks.
    for(unsigned level=0;level<4U;++level) {
        CompressedTextureMip mip;
        mip.width=w;mip.height=h;
        mip.blocks.resize(rhi::texture_row_bytes(c.format,w)*rhi::texture_rows(c.format,h));
        for(std::size_t i=0;i<mip.blocks.size();++i)
            mip.blocks[i]=static_cast<std::byte>((i*29U+level*31U+c.vk)&255U);
        input.mips.push_back(std::move(mip));
        w=std::max(1U,w/2U);h=std::max(1U,h/2U);
    }
    std::string error;
    std::vector<std::byte> written,second;
    check(encode_native_ktx2(input,written,&error),"libktx writer rejected valid mip chain: "+error);
    check(encode_native_ktx2(input,second,&error),"repeat writer failed: "+error);
    check(written==second,"libktx native export not deterministic");

    // This is an independent reader, not just a DVE parser roundtrip.
    ktxTexture2* raw=nullptr;
    const auto parsed=ktxTexture2_CreateFromMemory(
        reinterpret_cast<const ktx_uint8_t*>(written.data()),written.size(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,&raw);
    check(parsed==KTX_SUCCESS && raw!=nullptr,
          "independent libktx reader rejected DVE-exported KTX2");
    std::unique_ptr<ktxTexture2,void(*)(ktxTexture2*)> holder(
        raw,[](ktxTexture2* ptr){ktxTexture_Destroy(ktxTexture(ptr));});
    check(raw->vkFormat==c.vk && raw->baseWidth==c.w && raw->baseHeight==c.h,
          "libktx read incorrect dimensions or VkFormat");
    check(raw->numDimensions==2U && raw->numLevels==input.mips.size() &&
          raw->numFaces==1U && raw->supercompressionScheme==KTX_SS_NONE,
          "libktx read unexpected layout or supercompression");
    const auto* imageData=ktxTexture_GetData(ktxTexture(raw));
    check(imageData!=nullptr,"libktx did not load image data");
    for(std::size_t level=0;level<input.mips.size();++level) {
        ktx_size_t offset=0U;
        check(ktxTexture2_GetImageOffset(raw,static_cast<ktx_uint32_t>(level),0U,0U,
                                       &offset)==KTX_SUCCESS,
              "libktx image level offset lookup failed");
        const auto& expected=input.mips[level].blocks;
        check(offset<=raw->dataSize && expected.size()<=raw->dataSize-offset,
              "libktx mip offset out of bounds");
        check(std::memcmp(imageData+offset,expected.data(),expected.size())==0,
              "independent libktx reader mip payload mismatch");
    }
    CompressedTextureAsset decoded;
    check(parse_compressed_texture(written,"native-ktx2",decoded,&error),
          "DVE reader rejected independently validated libktx file: "+error);
    check(decoded.format==c.format && decoded.mips.size()==input.mips.size(),
          "DVE native KTX2 metadata mismatch");
    for(std::size_t level=0;level<input.mips.size();++level)
        check(decoded.mips[level].blocks==input.mips[level].blocks,
              "DVE native KTX2 mip payload differs");

    // Corrupted/truncated containers must not be accepted by either loader.
    auto truncated=written;truncated.pop_back();
    check(!parse_compressed_texture(truncated,"truncated",decoded,&error),
          "DVE accepted truncated KTX2");
    auto bad=input;bad.mips[0].blocks.pop_back();
    std::vector<std::byte> invalid;
    check(!encode_native_ktx2(bad,invalid,&error)&&invalid.empty(),
          "libktx exporter accepted malformed native mip");
    std::cout<<"independent libktx roundtrip VkFormat="<<c.vk
             <<" dimensions="<<c.w<<"x"<<c.h
             <<" mips="<<input.mips.size()<<" size="<<written.size()<<"\n";
}
}
int main() {try {
    for(const auto c:{
        Case{rhi::TextureFormat::BC5RGUnorm,141U,13U,9U},
        Case{rhi::TextureFormat::BC7RGBAUnorm,145U,7U,5U},
        Case{rhi::TextureFormat::BC7RGBASrgb,146U,7U,5U},
        Case{rhi::TextureFormat::ASTC6x6RGBAUnorm,165U,17U,11U},
        Case{rhi::TextureFormat::ASTC8x8RGBASrgb,172U,17U,11U}
    }) verify(c);
    std::cout<<"T5 independent libktx KTX2 interoperability: PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
