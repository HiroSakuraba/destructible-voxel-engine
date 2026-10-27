#include "dve/render/bc7f_texture_encoder.hpp"
#include "basisu_transcoder.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace dve::render {
namespace {
std::once_flag g_init;
std::mutex g_encoderMutex;

void fail(std::string* error, const char* reason) {
    if (error) *error = reason;
}
bool encode_mips(const PolygonTextureMipChain& chain,
                 std::vector<std::vector<std::byte>>& blocks,
                 std::string* error, Bc7fQuality quality) {
    blocks.clear();
    if (chain.levels.empty() || chain.levels.size() > 32U) {
        fail(error, "bc7f requires a nonempty bounded mip chain"); return false;
    }
    std::call_once(g_init, [] { basist::basisu_transcoder_init(); });
    const auto flags = quality == Bc7fQuality::PartiallyAnalytical
        ? basist::bc7f::cPackBC7FlagDefaultPartiallyAnalytical
        : basist::bc7f::cPackBC7FlagDefault;
    std::vector<std::vector<std::byte>> result;
    result.reserve(chain.levels.size());
    for (const auto& mip : chain.levels) {
        if (mip.width == 0U || mip.height == 0U ||
            mip.width > 16384U || mip.height > 16384U ||
            mip.rgba8.size() != static_cast<std::size_t>(mip.width) * mip.height * 4U) {
            fail(error, "bc7f mip dimension or source bytes invalid"); return false;
        }
        const auto tilesX = (static_cast<std::size_t>(mip.width) + 3U) / 4U;
        const auto tilesY = (static_cast<std::size_t>(mip.height) + 3U) / 4U;
        if (tilesX > std::numeric_limits<std::size_t>::max() / tilesY / 16U) {
            fail(error, "bc7f output would overflow"); return false;
        }
        std::vector<std::byte> encoded(tilesX * tilesY * 16U);
        std::array<basist::color_rgba, 16> pixels{};
        std::array<std::uint8_t, 16> block{};
        // A serialized encoder protects upstream's internal counters and cache
        // initialization; callers can submit preparation jobs concurrently.
        std::scoped_lock lock(g_encoderMutex);
        for (std::size_t y=0; y<tilesY; ++y) {
            for (std::size_t x=0; x<tilesX; ++x) {
                for (std::size_t py=0; py<4U; ++py) {
                    for (std::size_t px=0; px<4U; ++px) {
                        const std::size_t sx=std::min<std::size_t>(x*4U+px,mip.width-1U);
                        const std::size_t sy=std::min<std::size_t>(y*4U+py,mip.height-1U);
                        const std::size_t src=(sy*mip.width+sx)*4U;
                        auto& p=pixels[py*4U+px];
                        p.r=mip.rgba8[src];
                        p.g=mip.rgba8[src+1U];
                        p.b=mip.rgba8[src+2U];
                        p.a=mip.rgba8[src+3U];
                    }
                }
                (void)basist::bc7f::fast_pack_bc7_auto_rgba(block.data(),pixels.data(),flags);
                std::memcpy(encoded.data()+(y*tilesX+x)*16U,block.data(),block.size());
            }
        }
        result.push_back(std::move(encoded));
    }
    blocks=std::move(result);
    return true;
}
} // namespace

TextureBc7Encoder make_basis_bc7f_encoder(Bc7fQuality quality) {
    return [quality](const PolygonTextureMipChain& chain,
                     std::vector<std::vector<std::byte>>& out,
                     std::string* error) {
        return encode_mips(chain,out,error,quality);
    };
}
} // namespace dve::render
