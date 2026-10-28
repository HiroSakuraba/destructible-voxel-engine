// T1 proof of concept: direct, pinned Basis Universal bc7f low-level encoder.
// Not used by the DVE runtime/asset cooker. The upstream API is internal.
#include "basisu_transcoder.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    basist::basisu_transcoder_init();
    constexpr std::size_t kBlocks = 4096;
    using Clock = std::chrono::steady_clock;
    for (const auto flags : {basist::bc7f::cPackBC7FlagDefault,
                              basist::bc7f::cPackBC7FlagDefaultPartiallyAnalytical}) {
        // Fixtures, the determinism re-encode, SSE and checksum are outside the timed region:
        // ns_per_block measures exactly one fast_pack_bc7_auto_rgba() call per counted block.
        std::vector<std::array<basist::color_rgba, 16>> fixtures(kBlocks);
        for (std::size_t b = 0; b < kBlocks; ++b) {
            for (std::size_t i=0;i<fixtures[b].size();++i) {
                const std::uint32_t x=static_cast<std::uint32_t>(i%4U);
                const std::uint32_t y=static_cast<std::uint32_t>(i/4U);
                const std::uint32_t seed=static_cast<std::uint32_t>(b);
                fixtures[b][i].r=static_cast<std::uint8_t>((x*63U+seed*7U)&255U);
                fixtures[b][i].g=static_cast<std::uint8_t>((y*71U+seed*3U)&255U);
                fixtures[b][i].b=static_cast<std::uint8_t>(((x+y)*29U+seed*11U)&255U);
                fixtures[b][i].a=static_cast<std::uint8_t>((seed%3U)==0U?255U:((x*47U+y*41U+seed)&255U));
            }
        }
        std::vector<std::array<std::uint8_t, 16>> blocks(kBlocks);
        const auto start = Clock::now();
        for (std::size_t b = 0; b < kBlocks; ++b)
            (void)basist::bc7f::fast_pack_bc7_auto_rgba(blocks[b].data(),fixtures[b].data(),flags);
        const auto us=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
        std::uint64_t accumulatedError = 0;
        std::uint64_t checksum = 1469598103934665603ULL;
        for (std::size_t b = 0; b < kBlocks; ++b) {
            std::array<std::uint8_t, 16> repeat{};
            (void)basist::bc7f::fast_pack_bc7_auto_rgba(repeat.data(),fixtures[b].data(),flags);
            if (blocks[b]!=repeat) throw std::runtime_error("bc7f generated non-deterministic block");
            // BC7 mode = index of the lowest set bit of byte 0; byte 0 == 0 is reserved/invalid.
            if (blocks[b][0]==0U) throw std::runtime_error("bc7f invalid mode prefix");
            accumulatedError+=basist::bc7f::calc_sse(blocks[b].data(),fixtures[b].data());
            for (auto v:blocks[b]) checksum=(checksum^v)*1099511628211ULL;
        }
        const double mse=static_cast<double>(accumulatedError)/(static_cast<double>(kBlocks)*16.0*4.0);
        const double psnr=mse>0.0?10.0*std::log10((255.0*255.0)/mse):99.0;
        std::cout<<"{\"encoder\":\"bc7f\",\"mode\":\""
                 <<(flags==basist::bc7f::cPackBC7FlagDefault?"analytical":"partially_analytical")
                 <<"\",\"blocks\":"<<kBlocks<<",\"ns_per_block\":"<<(us*1000.0/kBlocks)
                 <<",\"psnr_db\":"<<psnr<<",\"checksum\":"<<checksum<<"}\n";
    }
    return 0;
}
