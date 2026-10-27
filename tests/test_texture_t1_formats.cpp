#include "dve/compressed_texture.hpp"
#include "dve/rhi/null_device.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
namespace {
void require(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
void u32(std::vector<std::byte>& a, std::size_t i, std::uint32_t v) {
    for (std::size_t b=0;b<4;++b) a.at(i+b)=static_cast<std::byte>((v>>(b*8U)) & 255U);
}
void u64(std::vector<std::byte>& a, std::size_t i, std::uint64_t v) {
    u32(a,i,static_cast<std::uint32_t>(v));
    u32(a,i+4,static_cast<std::uint32_t>(v>>32U));
}
std::vector<std::byte> ktx(std::uint32_t vkFormat, std::uint32_t width,
                           std::uint32_t height, std::size_t size) {
    std::vector<std::byte> b(112U+size);
    constexpr unsigned char magic[12] = {0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A};
    for(std::size_t i=0;i<12;++i)b[i]=static_cast<std::byte>(magic[i]);
    u32(b,12,vkFormat);u32(b,16,1);u32(b,20,width);u32(b,24,height);
    u32(b,36,1);u32(b,40,1);u32(b,48,104);u32(b,52,4);
    u64(b,80,112);u64(b,88,size);u64(b,96,size);
    for(std::size_t i=112;i<b.size();++i)b[i]=static_cast<std::byte>((i*17U)&255U);
    return b;
}
std::vector<std::byte> dds(std::uint32_t dxgi, std::uint32_t width,
                           std::uint32_t height, std::size_t size) {
    std::vector<std::byte> b(148U+size);
    u32(b,0,0x20534444U);u32(b,4,124);u32(b,8,0x21007);
    u32(b,12,height);u32(b,16,width);u32(b,76,32);u32(b,80,4);
    u32(b,84,0x30315844U);u32(b,108,0x1000);
    u32(b,128,dxgi);u32(b,132,3);u32(b,140,1);
    for(std::size_t i=148;i<b.size();++i)b[i]=static_cast<std::byte>((i*5U)&255U);
    return b;
}
void verify(std::vector<std::byte> encoded, rhi::TextureFormat format,
            std::uint32_t width, std::uint32_t height) {
    std::string error;
    CompressedTextureAsset asset;
    require(parse_compressed_texture(encoded,"T1",asset,&error),"T1 parser rejected valid texture");
    require(asset.format==format && asset.mips.size()==1,"T1 parsed format mismatch");
    const auto expected=rhi::texture_row_bytes(format,width)*rhi::texture_rows(format,height);
    require(expected == asset.mips[0].blocks.size(),"T1 block extent/payload mismatch");
    rhi::NullDevice device;
    require(device.texture_format_capabilities(format).sampled,"T1 null format capability");
    CompressedTextureResource gpu;
    require(upload_compressed_texture(device,asset,gpu,&error),"T1 Null upload failed");
    require(device.statistics().uploadedBytes==expected,"T1 Null uploaded bytes");
    destroy_compressed_texture(device,gpu);

    rhi::TextureDesc desc;
    desc.format=format;desc.width=width;desc.height=height;
    desc.usage=rhi::TextureUsage::CopyDestination|rhi::TextureUsage::CopySource|rhi::TextureUsage::Sampled;
    auto tex=device.create_texture(desc,&error);
    require(static_cast<bool>(tex),"T1 direct copy texture allocation failed");
    require(device.write_texture(tex,0,0,asset.mips[0].blocks,rhi::texture_row_bytes(format,width),&error),
            "T1 direct compressed write failed");
    std::vector<std::byte> readback(expected);
    require(device.read_texture(tex,0,0,readback,rhi::texture_row_bytes(format,width),&error),
            "T1 compressed readback failed");
    require(readback==asset.mips[0].blocks,"T1 block bytes changed during Null roundtrip");
    require(device.destroy_texture(tex,&error),"T1 texture destroy");
    auto bad=std::move(encoded);bad.pop_back();
    require(!parse_compressed_texture(bad,"bad",asset,&error),"T1 accepted truncated payload");
}
}
int main() {
    // All supported T1 block sizes and formats; 7x7 and 17x9 probe edge blocks.
    struct Sample { std::uint32_t vk; rhi::TextureFormat f; };
    constexpr Sample samples[] = {
        {139,rhi::TextureFormat::BC4RUnorm},
        {143,rhi::TextureFormat::BC6HRGBUfloat},
        {144,rhi::TextureFormat::BC6HRGBSfloat},
        {145,rhi::TextureFormat::BC7RGBAUnorm},
        {146,rhi::TextureFormat::BC7RGBASrgb},
        {147,rhi::TextureFormat::ETC2RGBUnorm},
        {148,rhi::TextureFormat::ETC2RGBSrgb},
        {151,rhi::TextureFormat::ETC2RGBAUnorm},
        {152,rhi::TextureFormat::ETC2RGBASrgb},
        {153,rhi::TextureFormat::EACR11Unorm},
        {155,rhi::TextureFormat::EACRG11Unorm},
        {157,rhi::TextureFormat::ASTC4x4RGBAUnorm},
        {158,rhi::TextureFormat::ASTC4x4RGBASrgb},
        {165,rhi::TextureFormat::ASTC6x6RGBAUnorm},
        {166,rhi::TextureFormat::ASTC6x6RGBASrgb},
        {171,rhi::TextureFormat::ASTC8x8RGBAUnorm},
        {172,rhi::TextureFormat::ASTC8x8RGBASrgb}
    };
    for(const auto& s:samples) {
        const std::size_t size=rhi::texture_row_bytes(s.f,7)*rhi::texture_rows(s.f,7);
        verify(ktx(s.vk,7,7,size),s.f,7,7);
        require(rhi::texture_block_bytes(s.f)!=0U,"T1 missing block byte size");
    }
    verify(dds(80,7,7,32),rhi::TextureFormat::BC4RUnorm,7,7);
    verify(dds(95,7,7,64),rhi::TextureFormat::BC6HRGBUfloat,7,7);
    verify(dds(98,7,7,64),rhi::TextureFormat::BC7RGBAUnorm,7,7);
    verify(dds(99,7,7,64),rhi::TextureFormat::BC7RGBASrgb,7,7);
    require(rhi::texture_row_bytes(rhi::TextureFormat::ASTC8x8RGBAUnorm,17)==48U,
            "T1 ASTC 8x8 row pitch");
    verify(ktx(171,17,9,96),rhi::TextureFormat::ASTC8x8RGBAUnorm,17,9);
    // Reject KTX2 supercompression without a transcoder, not misread as native blocks.
    auto super=ktx(145,4,4,16);
    u32(super,44,2);
    CompressedTextureAsset output;
    std::string error;
    require(!parse_compressed_texture(super,"zstd",output,&error),"T1 accepted supercompressed KTX2");
    require(!rhi::is_block_compressed(rhi::TextureFormat::RGBA8Unorm),
            "T1 uncompressed format misidentified");
}
