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
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
void check(bool pass,const std::string& msg) {if(!pass)throw std::runtime_error(msg);}

// Everything the independent checks need to know about a format, written out by hand from the
// KTX2 / Vulkan specifications instead of derived from DVE's own helpers.
struct Case {
    const char* label;
    rhi::TextureFormat format;
    std::uint32_t vk;          // VkFormat enum value
    std::uint32_t blockW,blockH;
    khr_df_model_e model;
    khr_df_transfer_e transfer; // KHR_DF_TRANSFER_SRGB for *_SRGB_BLOCK, else LINEAR
    std::uint32_t w,h;
    std::uint32_t levels;      // 0 = complete chain down to 1x1
};
std::uint32_t full_chain(std::uint32_t w,std::uint32_t h) {
    std::uint32_t levels=1U;
    for(auto extent=std::max(w,h);extent>1U;extent>>=1U)++levels;
    return levels;
}
std::uint32_t read32(const std::vector<std::byte>& bytes,std::size_t at) {
    check(at+4U<=bytes.size(),"KTX2 header read out of range");
    std::uint32_t v=0U;
    for(unsigned i=0;i<4U;++i)v|=static_cast<std::uint32_t>(bytes[at+i])<<(8U*i);
    return v;
}
std::uint64_t read64(const std::vector<std::byte>& bytes,std::size_t at) {
    return static_cast<std::uint64_t>(read32(bytes,at))|
           static_cast<std::uint64_t>(read32(bytes,at+4U))<<32U;
}
void fill(CompressedTextureAsset& input,const Case& c,std::uint32_t levels) {
    auto w=c.w,h=c.h;
    for(unsigned level=0;level<levels;++level) {
        CompressedTextureMip mip;
        mip.width=w;mip.height=h;
        mip.blocks.resize(rhi::texture_row_bytes(c.format,w)*rhi::texture_rows(c.format,h));
        for(std::size_t i=0;i<mip.blocks.size();++i)
            mip.blocks[i]=static_cast<std::byte>((i*29U+level*31U+c.vk)&255U);
        input.mips.push_back(std::move(mip));
        w=std::max(1U,w/2U);h=std::max(1U,h/2U);
    }
}
void verify(const Case& c) {
    const auto levels=c.levels?c.levels:full_chain(c.w,c.h);
    check(levels<=full_chain(c.w,c.h),std::string(c.label)+": bad fixture, more levels than a 2D chain allows");
    CompressedTextureAsset input;
    input.name="KTX2 independent validation";input.format=c.format;
    fill(input,c,levels);
    std::string error;
    std::vector<std::byte> written,second;
    check(encode_native_ktx2(input,written,&error),
          std::string(c.label)+": libktx writer rejected valid mip chain: "+error);
    check(encode_native_ktx2(input,second,&error),"repeat writer failed: "+error);
    check(written==second,"libktx native export not deterministic");

    // Block-padded payload sizes from the format's block dimensions (each block is 16 bytes for
    // BC5/BC7/ASTC), so a wrong DVE size helper cannot agree with itself.
    std::vector<std::size_t> expectedSize;
    for(std::uint32_t level=0,w=c.w,h=c.h;level<levels;++level) {
        expectedSize.push_back(static_cast<std::size_t>((w+c.blockW-1U)/c.blockW)*
                               ((h+c.blockH-1U)/c.blockH)*16U);
        check(input.mips[level].blocks.size()==expectedSize.back(),
              std::string(c.label)+": DVE block padding disagrees with the format specification");
        w=std::max(1U,w/2U);h=std::max(1U,h/2U);
    }

    // Raw KTX2 header, level index and DFD fields straight from the bytes (KTX 2.0 specification).
    static constexpr std::uint8_t identifier[12]={0xAB,'K','T','X',' ','2','0',0xBB,'\r','\n',0x1A,'\n'};
    check(written.size()>=80U+24U*levels,"KTX2 file shorter than its header and level index");
    for(std::size_t i=0;i<12U;++i)
        check(static_cast<std::uint8_t>(written[i])==identifier[i],"KTX2 file identifier mismatch");
    check(read32(written,12U)==c.vk,"KTX2 header vkFormat mismatch");
    check(read32(written,16U)==1U,"KTX2 header typeSize must be 1 for block-compressed data");
    check(read32(written,20U)==c.w&&read32(written,24U)==c.h,"KTX2 header pixel size mismatch");
    check(read32(written,28U)==0U&&read32(written,32U)==0U,"KTX2 header must have no depth or layers");
    check(read32(written,36U)==1U,"KTX2 header faceCount must be 1");
    check(read32(written,40U)==levels,"KTX2 header levelCount mismatch");
    check(read32(written,44U)==0U,"KTX2 header supercompressionScheme must be none");
    const auto dfdOffset=read32(written,48U),dfdLength=read32(written,52U);
    check(dfdOffset==80U+24U*levels&&dfdLength>=44U&&dfdOffset+dfdLength<=written.size(),
          "KTX2 data format descriptor is not placed after the level index");
    check(read32(written,dfdOffset)==dfdLength,"KTX2 DFD totalSize disagrees with the header");
    check(static_cast<std::uint8_t>(written[dfdOffset+12U])==static_cast<std::uint8_t>(c.model),
          "KTX2 DFD color model mismatch");
    check(static_cast<std::uint8_t>(written[dfdOffset+14U])==static_cast<std::uint8_t>(c.transfer),
          "KTX2 DFD transfer function mismatch (sRGB vs linear)");
    std::uint64_t previousOffset=std::numeric_limits<std::uint64_t>::max();
    for(std::uint32_t level=0;level<levels;++level) {
        const auto entry=80U+24U*static_cast<std::size_t>(level);
        const auto offset=read64(written,entry),length=read64(written,entry+8U),
                   uncompressed=read64(written,entry+16U);
        check(length==expectedSize[level]&&uncompressed==length,
              "KTX2 level index byteLength mismatch");
        check(offset%16U==0U&&offset+length<=written.size(),
              "KTX2 level offset is misaligned or out of range");
        check(offset<previousOffset,"KTX2 levels must be stored smallest mip first");
        previousOffset=offset;
    }

    // This is an independent reader, not just a DVE parser roundtrip.
    ktxTexture2* raw=nullptr;
    const auto parsed=ktxTexture2_CreateFromMemory(
        reinterpret_cast<const ktx_uint8_t*>(written.data()),written.size(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,&raw);
    check(parsed==KTX_SUCCESS && raw!=nullptr,
          std::string("independent libktx reader rejected DVE-exported KTX2: ")+ktxErrorString(parsed));
    std::unique_ptr<ktxTexture2,void(*)(ktxTexture2*)> holder(
        raw,[](ktxTexture2* ptr){ktxTexture_Destroy(ktxTexture(ptr));});
    check(raw->vkFormat==c.vk && raw->baseWidth==c.w && raw->baseHeight==c.h,
          "libktx read incorrect dimensions or VkFormat");
    check(raw->numDimensions==2U && raw->numLevels==levels &&
          raw->numFaces==1U && raw->numLayers==1U && !raw->isArray &&
          raw->supercompressionScheme==KTX_SS_NONE && !ktxTexture2_NeedsTranscoding(raw),
          "libktx read unexpected layout or supercompression");
    check(ktxTexture2_GetColorModel_e(raw)==c.model,"libktx read the wrong DFD color model");
    check(ktxTexture2_GetOETF_e(raw)==c.transfer,
          "libktx read the wrong DFD transfer function (sRGB vs linear)");
    const auto* imageData=ktxTexture_GetData(ktxTexture(raw));
    check(imageData!=nullptr,"libktx did not load image data");
    for(std::size_t level=0;level<input.mips.size();++level) {
        ktx_size_t offset=0U;
        check(ktxTexture2_GetImageOffset(raw,static_cast<ktx_uint32_t>(level),0U,0U,
                                       &offset)==KTX_SUCCESS,
              "libktx image level offset lookup failed");
        const auto& expected=input.mips[level].blocks;
        check(ktxTexture_GetImageSize(ktxTexture(raw),static_cast<ktx_uint32_t>(level))==expected.size(),
              "libktx level image size differs from the exported compressed bytes");
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
    ktxTexture2* rejected=nullptr;
    check(ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(truncated.data()),
              truncated.size(),KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,&rejected)!=KTX_SUCCESS,
          "libktx accepted truncated KTX2");
    auto bad=input;bad.mips[0].blocks.pop_back();
    std::vector<std::byte> invalid;
    check(!encode_native_ktx2(bad,invalid,&error)&&invalid.empty(),
          "libktx exporter accepted malformed native mip");
    std::cout<<"independent libktx roundtrip "<<c.label<<" VkFormat="<<c.vk
             <<" dimensions="<<c.w<<"x"<<c.h
             <<" mips="<<input.mips.size()<<" size="<<written.size()<<"\n";
}
// A chain longer than log2(max extent)+1 is not a valid texture. libktx refuses it, and the
// exporter must say so itself instead of returning libktx's bare failure.
void verify_overlong_chain_rejected() {
    CompressedTextureAsset input;
    input.name="overlong chain";input.format=rhi::TextureFormat::BC7RGBAUnorm;
    const Case c{"overlong",input.format,145U,4U,4U,KHR_DF_MODEL_BC7,KHR_DF_TRANSFER_LINEAR,7U,5U,0U};
    fill(input,c,4U); // 7x5, 3x2, 1x1, 1x1
    std::string error;std::vector<std::byte> bytes;
    check(!encode_native_ktx2(input,bytes,&error)&&bytes.empty(),
          "exporter accepted a mip chain longer than the texture allows");
    check(error.find("mip count")!=std::string::npos,"overlong chain error is not descriptive: "+error);
    std::cout<<"overlong mip chain rejected: "<<error<<"\n";
}
}
int main() {try {
    const auto lin=KHR_DF_TRANSFER_LINEAR;
    const auto srgb=KHR_DF_TRANSFER_SRGB;
    for(const auto& c:{
        // Complete chains, including 1x1 and 2x2 tails that are padded to a whole 4x4 block.
        Case{"BC5 13x9",rhi::TextureFormat::BC5RGUnorm,141U,4U,4U,KHR_DF_MODEL_BC5,lin,13U,9U,0U},
        Case{"BC5 4x4",rhi::TextureFormat::BC5RGUnorm,141U,4U,4U,KHR_DF_MODEL_BC5,lin,4U,4U,0U},
        Case{"BC5 64x64 (baked size)",rhi::TextureFormat::BC5RGUnorm,141U,4U,4U,KHR_DF_MODEL_BC5,lin,64U,64U,0U},
        Case{"BC7 UNORM 7x5",rhi::TextureFormat::BC7RGBAUnorm,145U,4U,4U,KHR_DF_MODEL_BC7,lin,7U,5U,0U},
        Case{"BC7 sRGB 7x5",rhi::TextureFormat::BC7RGBASrgb,146U,4U,4U,KHR_DF_MODEL_BC7,srgb,7U,5U,0U},
        // A chain that stops before 1x1 is legal.
        Case{"BC7 UNORM 33x17, 3 of 6 levels",rhi::TextureFormat::BC7RGBAUnorm,145U,4U,4U,KHR_DF_MODEL_BC7,lin,33U,17U,3U},
        Case{"ASTC 6x6 UNORM 17x11",rhi::TextureFormat::ASTC6x6RGBAUnorm,165U,6U,6U,KHR_DF_MODEL_ASTC,lin,17U,11U,0U},
        Case{"ASTC 8x8 sRGB 17x11",rhi::TextureFormat::ASTC8x8RGBASrgb,172U,8U,8U,KHR_DF_MODEL_ASTC,srgb,17U,11U,0U}
    }) verify(c);
    verify_overlong_chain_rejected();
    std::cout<<"T5 independent libktx KTX2 interoperability: PASS\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
