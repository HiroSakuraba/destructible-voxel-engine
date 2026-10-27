#include "dve/render/normal_bc5_codec.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dve::render {
namespace {
void set_error(std::string* error, const char* message) {
    if (error) *error=message;
}
using Block = std::array<std::uint8_t,8>;

std::array<std::uint8_t,8> palette(std::uint8_t hi, std::uint8_t lo) noexcept {
    std::array<std::uint8_t,8> p{hi,lo};
    if (hi > lo) {
        for (std::uint32_t i=1;i<=6;++i)
            p[i+1U]=static_cast<std::uint8_t>(
                ((7U-i)*static_cast<unsigned>(hi)+i*static_cast<unsigned>(lo)+3U)/7U);
    } else {
        for (std::uint32_t i=1;i<=4;++i)
            p[i+1U]=static_cast<std::uint8_t>(
                ((5U-i)*static_cast<unsigned>(hi)+i*static_cast<unsigned>(lo)+2U)/5U);
        p[6]=0U; p[7]=255U;
    }
    return p;
}
Block pack_bc4(const std::array<std::uint8_t,16>& source) noexcept {
    const auto [minIt,maxIt]=std::minmax_element(source.begin(),source.end());
    const auto hi=*maxIt,lo=*minIt;
    const auto p=palette(hi,lo);
    Block result{};
    result[0]=hi; result[1]=lo;
    std::uint64_t bits=0U;
    for (std::size_t i=0;i<16U;++i) {
        std::uint32_t best=0U; unsigned bestErr=std::numeric_limits<unsigned>::max();
        for (std::uint32_t candidate=0;candidate<8U;++candidate) {
            const int diff=static_cast<int>(source[i])-static_cast<int>(p[candidate]);
            const auto err=static_cast<unsigned>(diff*diff);
            if (err < bestErr) { bestErr=err; best=candidate; }
        }
        bits|=static_cast<std::uint64_t>(best)<<(3U*i);
    }
    for (std::size_t i=0;i<6U;++i)
        result[i+2U]=static_cast<std::uint8_t>((bits>>(8U*i))&255U);
    return result;
}
std::array<std::uint8_t,16> unpack_bc4(std::span<const std::byte,8> block) noexcept {
    std::array<std::uint8_t,16> result{};
    const auto p=palette(std::to_integer<std::uint8_t>(block[0]),
                         std::to_integer<std::uint8_t>(block[1]));
    std::uint64_t bits=0U;
    for (std::size_t i=0;i<6U;++i)
        bits|=static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(block[i+2U]))<<(8U*i);
    for (std::size_t i=0;i<16U;++i) result[i]=p[(bits>>(3U*i))&7U];
    return result;
}
std::array<double,3> normal(std::uint8_t x,std::uint8_t y,std::uint8_t z) noexcept {
    double nx=(static_cast<double>(x)/127.5)-1.0;
    double ny=(static_cast<double>(y)/127.5)-1.0;
    double nz=(static_cast<double>(z)/127.5)-1.0;
    const double length=std::sqrt(nx*nx+ny*ny+nz*nz);
    if (length<1e-12) return {0.0,0.0,1.0};
    return {nx/length,ny/length,nz/length};
}
std::array<double,3> normal_bc5(std::uint8_t x,std::uint8_t y) noexcept {
    double nx=(static_cast<double>(x)/127.5)-1.0;
    double ny=(static_cast<double>(y)/127.5)-1.0;
    const double xy2=nx*nx+ny*ny;
    const double z=std::sqrt(std::max(0.0,1.0-xy2));
    const double len=std::sqrt(xy2+z*z);
    if (len<1e-12) return {0.0,0.0,1.0};
    return {nx/len,ny/len,z/len};
}
} // namespace

bool encode_bc5_normal_mips(const PolygonTextureMipChain& source,
                            std::vector<std::vector<std::byte>>& blocks,
                            std::string* error) {
    blocks.clear();
    if (source.levels.empty() || source.levels.size()>32U) {
        set_error(error,"BC5 requires a nonempty mip chain"); return false;
    }
    std::vector<std::vector<std::byte>> output;
    output.reserve(source.levels.size());
    for (const auto& level:source.levels) {
        if (!level.width || !level.height || level.width>16384U || level.height>16384U ||
            level.rgba8.size()!=static_cast<std::size_t>(level.width)*level.height*4U) {
            set_error(error,"BC5 invalid mip dimensions or RGBA bytes"); return false;
        }
        // BC5 has no blue channel/sign bit: do not silently destroy back-facing tangent normals.
        for (std::size_t i=2U;i<level.rgba8.size();i+=4U) {
            if (level.rgba8[i]<128U) {
                set_error(error,"BC5 cannot store a negative-Z tangent normal"); return false;
            }
        }
        const auto nx=(static_cast<std::size_t>(level.width)+3U)/4U;
        const auto ny=(static_cast<std::size_t>(level.height)+3U)/4U;
        std::vector<std::byte> encoded(nx*ny*16U);
        for (std::size_t by=0;by<ny;++by) for (std::size_t bx=0;bx<nx;++bx) {
            std::array<std::uint8_t,16> r{},g{};
            for (std::size_t py=0;py<4U;++py) for (std::size_t px=0;px<4U;++px) {
                const auto x=std::min<std::size_t>(bx*4U+px,level.width-1U);
                const auto y=std::min<std::size_t>(by*4U+py,level.height-1U);
                const auto index=(y*level.width+x)*4U;
                r[py*4U+px]=level.rgba8[index];
                g[py*4U+px]=level.rgba8[index+1U];
            }
            const auto rb=pack_bc4(r),gb=pack_bc4(g);
            const auto offset=(by*nx+bx)*16U;
            for (std::size_t i=0;i<8U;++i) {
                encoded[offset+i]=static_cast<std::byte>(rb[i]);
                encoded[offset+8U+i]=static_cast<std::byte>(gb[i]);
            }
        }
        output.push_back(std::move(encoded));
    }
    blocks=std::move(output);
    return true;
}

std::array<std::array<std::uint8_t,2>,16> decode_bc5_rg_block(
    std::span<const std::byte,16> block) noexcept {
    const auto r=unpack_bc4(std::span<const std::byte,8>(block.data(),8U));
    const auto g=unpack_bc4(std::span<const std::byte,8>(block.data()+8U,8U));
    std::array<std::array<std::uint8_t,2>,16> result{};
    for (std::size_t i=0;i<16U;++i) result[i]={r[i],g[i]};
    return result;
}

double bc5_mean_normal_angle_degrees(
    const PolygonTextureMip& source, std::span<const std::byte> blocks) noexcept {
    if (!source.width || !source.height ||
        source.rgba8.size()!=static_cast<std::size_t>(source.width)*source.height*4U ||
        blocks.size()!=rhi::texture_row_bytes(rhi::TextureFormat::BC5RGUnorm,source.width)*
                       rhi::texture_rows(rhi::TextureFormat::BC5RGUnorm,source.height))
        return -1.0;
    const auto nx=(static_cast<std::size_t>(source.width)+3U)/4U;
    double sum=0.0;
    for (std::uint32_t y=0;y<source.height;++y) for (std::uint32_t x=0;x<source.width;++x) {
        const auto blockOffset=((static_cast<std::size_t>(y)/4U)*nx+x/4U)*16U;
        const auto decoded=decode_bc5_rg_block(
            std::span<const std::byte,16>(blocks.data()+blockOffset,16U));
        const auto p=decoded[(y%4U)*4U+(x%4U)];
        const auto original=(static_cast<std::size_t>(y)*source.width+x)*4U;
        const auto a=normal(source.rgba8[original],source.rgba8[original+1U],
                            source.rgba8[original+2U]);
        const auto b=normal_bc5(p[0],p[1]);
        const double dot=std::clamp(a[0]*b[0]+a[1]*b[1]+a[2]*b[2],-1.0,1.0);
        sum+=std::acos(dot)*(180.0/3.14159265358979323846);
    }
    return sum/(static_cast<double>(source.width)*source.height);
}

TextureBc7Encoder make_bc5_normal_encoder() {
    return [](const PolygonTextureMipChain& source,
              std::vector<std::vector<std::byte>>& output,
              std::string* error) {return encode_bc5_normal_mips(source,output,error);};
}
} // namespace dve::render
