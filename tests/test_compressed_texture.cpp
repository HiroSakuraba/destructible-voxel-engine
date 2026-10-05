#include "dve/compressed_texture.hpp"
#include "dve/rhi/null_device.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void put32(std::vector<std::byte>& b, std::size_t o, std::uint32_t v) {
    for (unsigned i=0;i<4;++i) b[o+i]=static_cast<std::byte>((v>>(i*8U))&255U);
}
void require(bool value, const char* message) {
    if (!value) { std::cerr<<message<<'\n'; std::exit(1); }
}
std::vector<std::byte> dds_fixture() {
    std::vector<std::byte> b(144);
    put32(b,0,0x20534444U); put32(b,4,124); put32(b,8,0x21007U);
    put32(b,12,4); put32(b,16,4); put32(b,28,2); put32(b,76,32);
    put32(b,80,4); put32(b,84,0x31545844U); put32(b,108,0x1000);
    for (std::size_t i=128;i<b.size();++i) b[i]=static_cast<std::byte>(i);
    return b;
}
}
int main() {
    dve::CompressedTextureAsset asset; std::string error;
    const auto bytes=dds_fixture();
    require(dve::parse_compressed_texture(bytes,"fixture.dds",asset,&error),error.c_str());
    require(asset.format==dve::rhi::TextureFormat::BC1RGBAUnorm && asset.mips.size()==2,
            "DDS BC1 mip metadata was not preserved");
    require(asset.mips[0].blocks.size()==8 && asset.mips[1].blocks.size()==8,
            "DDS BC1 payload size mismatch");
    auto truncated=bytes; truncated.pop_back();
    require(!dve::parse_compressed_texture(truncated,"short.dds",asset,&error),
            "truncated DDS mip payload was accepted");

    // KTX2 BC3 4x4, one level, no supercompression.
    std::vector<std::byte> ktx(128);
    const unsigned char id[12]={0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A};
    for (int i=0;i<12;++i) ktx[static_cast<std::size_t>(i)]=static_cast<std::byte>(id[i]);
    put32(ktx,12,137); put32(ktx,16,1); put32(ktx,20,4); put32(ktx,24,4);
    put32(ktx,36,1); put32(ktx,40,1);
    put32(ktx,48,104); put32(ktx,52,4); // bounded DFD payload
    // level index: byte offset 112, byte length 16, uncompressed length 16
    ktx[80]=std::byte{112}; ktx[88]=std::byte{16}; ktx[96]=std::byte{16};
    require(dve::parse_compressed_texture(ktx,"fixture.ktx2",asset,&error),error.c_str());
    require(asset.format==dve::rhi::TextureFormat::BC3RGBAUnorm && asset.mips.size()==1 &&
            asset.mips[0].blocks.size()==16,"KTX2 BC3 payload was not retained");

    dve::rhi::NullDevice device;
    dve::CompressedTextureResource resource;
    if (!dve::upload_compressed_texture(device,asset,resource,&error)) {
        std::cerr << "compressed texture upload through Null RHI failed: " << error << '\n';
        return 1;
    }
    require(device.statistics().uploadedBytes==16,"RHI did not upload the exact BC3 block payload size");
    dve::destroy_compressed_texture(device,resource);
    return 0;
}
