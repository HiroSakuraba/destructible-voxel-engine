#include "dve/compressed_texture.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace dve {
namespace {
constexpr std::uint64_t kMaximumBytes = 512ULL * 1024ULL * 1024ULL;
void fail(std::string* error, const char* message) { if (error) *error = message; }
std::uint32_t u32le(std::span<const std::byte> b, std::size_t o) {
    return std::to_integer<std::uint32_t>(b[o]) |
        (std::to_integer<std::uint32_t>(b[o+1]) << 8U) |
        (std::to_integer<std::uint32_t>(b[o+2]) << 16U) |
        (std::to_integer<std::uint32_t>(b[o+3]) << 24U);
}
std::uint64_t u64le(std::span<const std::byte> b, std::size_t o) {
    return static_cast<std::uint64_t>(u32le(b,o)) |
        (static_cast<std::uint64_t>(u32le(b,o+4)) << 32U);
}
rhi::TextureFormat dxgi(std::uint32_t f) {
    switch (f) {
    case 71: return rhi::TextureFormat::BC1RGBAUnorm;
    case 72: return rhi::TextureFormat::BC1RGBASrgb;
    case 77: return rhi::TextureFormat::BC3RGBAUnorm;
    case 78: return rhi::TextureFormat::BC3RGBASrgb;
    case 83: return rhi::TextureFormat::BC5RGUnorm;
    default: return static_cast<rhi::TextureFormat>(255);
    }
}
rhi::TextureFormat vkformat(std::uint32_t f) {
    switch (f) {
    case 131:
    case 133: return rhi::TextureFormat::BC1RGBAUnorm;
    case 132:
    case 134: return rhi::TextureFormat::BC1RGBASrgb;
    case 137: return rhi::TextureFormat::BC3RGBAUnorm;
    case 138: return rhi::TextureFormat::BC3RGBASrgb;
    case 141: return rhi::TextureFormat::BC5RGUnorm;
    default: return static_cast<rhi::TextureFormat>(255);
    }
}
std::size_t mip_bytes(rhi::TextureFormat f, std::uint32_t w, std::uint32_t h) {
    return static_cast<std::size_t>((w + 3U) / 4U) * ((h + 3U) / 4U) * rhi::texture_block_bytes(f);
}
bool append_mips(std::span<const std::byte> source, std::size_t offset,
                 std::uint32_t width, std::uint32_t height, std::uint32_t levels,
                 rhi::TextureFormat format, CompressedTextureAsset& out) {
    if (!width || !height || !levels || levels > 32U || width > 16384U || height > 16384U) return false;
    std::uint64_t total=0;
    for (std::uint32_t i=0;i<levels;++i) {
        const auto size=mip_bytes(format,width,height);
        if (size > kMaximumBytes-total || offset > source.size() || size > source.size()-offset) return false;
        CompressedTextureMip mip{width,height,{}};
        mip.blocks.assign(source.begin()+static_cast<std::ptrdiff_t>(offset),
                          source.begin()+static_cast<std::ptrdiff_t>(offset+size));
        out.mips.push_back(std::move(mip)); offset+=size; total+=size;
        width=std::max(1U,width/2U); height=std::max(1U,height/2U);
    }
    out.format=format;
    return true;
}
bool parse_dds(std::span<const std::byte> b, CompressedTextureAsset& out) {
    if (b.size()<128 || u32le(b,4)!=124 || u32le(b,76)!=32) return false;
    const std::uint32_t h=u32le(b,12), w=u32le(b,16), flags=u32le(b,8);
    const std::uint32_t pixelFlags=u32le(b,80);
    const std::uint32_t fourcc=u32le(b,84);
    constexpr std::uint32_t kFourCC=0x4U, kMipCount=0x20000U, kCube=0x200U, kVolume=0x200000U;
    if ((pixelFlags&kFourCC)==0 || (u32le(b,112)&(kCube|kVolume))!=0) return false;
    std::size_t offset=128;
    rhi::TextureFormat format=static_cast<rhi::TextureFormat>(255);
    if (fourcc==0x30315844U) { // DX10
        if (b.size()<148 || u32le(b,132)!=3 || u32le(b,140)!=1 || (u32le(b,136)&4U)!=0) return false;
        format=dxgi(u32le(b,128)); offset=148;
    } else if (fourcc==0x31545844U) format=rhi::TextureFormat::BC1RGBAUnorm;
    else if (fourcc==0x35545844U) format=rhi::TextureFormat::BC3RGBAUnorm;
    else if (fourcc==0x55354342U || fourcc==0x32495441U) format=rhi::TextureFormat::BC5RGUnorm;
    if (rhi::texture_block_bytes(format)==0) return false;
    return append_mips(b,offset,w,h,(flags&kMipCount)?std::max(1U,u32le(b,28)):1U,format,out);
}
bool parse_ktx2(std::span<const std::byte> b, CompressedTextureAsset& out) {
    constexpr std::array<unsigned char,12> id{0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A};
    if (b.size()<80) return false;
    for (std::size_t i=0;i<id.size();++i) if (std::to_integer<unsigned char>(b[i])!=id[i]) return false;
    const auto format=vkformat(u32le(b,12));
    const auto w=u32le(b,20), h=u32le(b,24), depth=u32le(b,28);
    const auto layers=u32le(b,32), faces=u32le(b,36), levels=std::max(1U,u32le(b,40));
    const std::uint64_t dfdOffset=u32le(b,48), dfdLength=u32le(b,52);
    if (rhi::texture_block_bytes(format)==0 || w==0 || h==0 || depth!=0 || (layers!=0 && layers!=1) || faces!=1 ||
        u32le(b,44)!=0 || levels>32 || b.size()<80U+static_cast<std::size_t>(levels)*24U ||
        dfdLength<4 || dfdOffset<80U+static_cast<std::uint64_t>(levels)*24U ||
        dfdOffset>b.size() || dfdLength>b.size()-dfdOffset) return false;
    std::uint32_t mw=w,mh=h;
    for (std::uint32_t i=0;i<levels;++i) {
        const std::size_t entry=80U+static_cast<std::size_t>(i)*24U;
        const auto off=u64le(b,entry), len=u64le(b,entry+8), raw=u64le(b,entry+16);
        const auto expected=mip_bytes(format,mw,mh);
        if (len!=expected || raw!=expected || off%8U!=0 || off<dfdOffset+dfdLength ||
            off>b.size() || len>b.size()-off) return false;
        CompressedTextureMip mip{mw,mh,{}};
        mip.blocks.assign(b.begin()+static_cast<std::ptrdiff_t>(off),b.begin()+static_cast<std::ptrdiff_t>(off+len));
        out.mips.push_back(std::move(mip)); mw=std::max(1U,mw/2U); mh=std::max(1U,mh/2U);
    }
    out.format=format; return true;
}
}

bool parse_compressed_texture(std::span<const std::byte> bytes, std::string name,
                              CompressedTextureAsset& output, std::string* error) {
    output={}; output.name=std::move(name);
    if (bytes.size()>kMaximumBytes) { fail(error,"compressed texture exceeds 512 MiB limit"); return false; }
    bool ok=false;
    if (bytes.size()>=4 && u32le(bytes,0)==0x20534444U) ok=parse_dds(bytes,output);
    else ok=parse_ktx2(bytes,output);
    if (!ok) { output={}; fail(error,"unsupported or invalid compressed texture; supported inputs are 2D BC1/BC3/BC5 DDS and un-supercompressed KTX2"); }
    return ok;
}

bool load_compressed_texture(const std::filesystem::path& path, CompressedTextureAsset& output,
                             std::string* error) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if (!in) { fail(error,"cannot open compressed texture"); return false; }
    const auto end=in.tellg(); if (end<0 || static_cast<std::uint64_t>(end)>kMaximumBytes) {
        fail(error,"compressed texture exceeds 512 MiB limit"); return false;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end)); in.seekg(0);
    if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) {
        fail(error,"cannot read compressed texture"); return false;
    }
    return parse_compressed_texture(bytes,path.filename().string(),output,error);
}

bool upload_compressed_texture(rhi::IDevice& device, const CompressedTextureAsset& asset,
                               CompressedTextureResource& output, std::string* error) {
    if (output.texture || output.view) { fail(error,"output texture resource must be empty before upload"); return false; }
    if (asset.mips.empty() || rhi::texture_block_bytes(asset.format)==0) { fail(error,"compressed texture asset is empty or has an unsupported format"); return false; }
    for (std::size_t i=0;i<asset.mips.size();++i) {
        const auto& mip=asset.mips[i];
        if (mip.width==0 || mip.height==0 || mip.blocks.size()!=mip_bytes(asset.format,mip.width,mip.height)) {
            fail(error,"compressed texture mip payload is invalid"); return false;
        }
    }
    if (!device.texture_format_capabilities(asset.format).sampled) {
        fail(error,"device does not support sampling this compressed texture format"); return false;
    }
    rhi::TextureDesc desc; desc.format=asset.format; desc.width=asset.mips[0].width;
    desc.height=asset.mips[0].height; desc.mipLevels=static_cast<std::uint32_t>(asset.mips.size());
    desc.usage=rhi::TextureUsage::Sampled|rhi::TextureUsage::CopyDestination;
    desc.initialState=rhi::ResourceState::ShaderRead; desc.debugName=asset.name;
    CompressedTextureResource staged; std::string local;
    staged.texture=device.create_texture(desc,&local);
    if (!staged.texture) { if(error)*error=local; return false; }
    for (std::uint32_t i=0;i<asset.mips.size();++i) {
        const auto& mip=asset.mips[i];
        if (!device.write_texture(staged.texture,i,0,mip.blocks,rhi::texture_row_bytes(asset.format,mip.width),&local)) {
            (void)device.destroy_texture(staged.texture,nullptr); if(error)*error=local; return false;
        }
    }
    rhi::TextureViewDesc view; view.texture=staged.texture; view.mipCount=desc.mipLevels;
    view.debugName=asset.name+" view"; staged.view=device.create_texture_view(view,&local);
    if (!staged.view) { (void)device.destroy_texture(staged.texture,nullptr); if(error)*error=local; return false; }
    output=staged; return true;
}

void destroy_compressed_texture(rhi::IDevice& device, CompressedTextureResource& resource) noexcept {
    if (resource.view) (void)device.destroy_texture_view(resource.view,nullptr);
    if (resource.texture) (void)device.destroy_texture(resource.texture,nullptr);
    resource={};
}
} // namespace dve
