// T1 proof of concept: direct, pinned Basis Universal bc7f low-level encoder.
// Not used by the DVE runtime/asset cooker. The upstream API is internal.
#include "basisu_transcoder.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    basist::basisu_transcoder_init();
    constexpr std::size_t kBlocks = 4096;
    using Clock = std::chrono::steady_clock;
    for (const auto flags : {basist::bc7f::cPackBC7FlagDefault,
                              basist::bc7f::cPackBC7FlagDefaultPartiallyAnalytical}) {
        std::uint64_t accumulatedError = 0;
        std::uint64_t checksum = 1469598103934665603ULL;
        const auto start = Clock::now();
        for (std::size_t b = 0; b < kBlocks; ++b) {
            std::array<basist::color_rgba, 16> pixels{};
            for (std::size_t i=0;i<pixels.size();++i) {
                const std::uint32_t x=static_cast<std::uint32_t>(i%4U);
                const std::uint32_t y=static_cast<std::uint32_t>(i/4U);
                pixels[i].r=static_cast<std::uint8_t>((x*63U+b*7U)&255U);
                pixels[i].g=static_cast<std::uint8_t>((y*71U+b*3U)&255U);
                pixels[i].b=static_cast<std::uint8_t>(((x+y)*29U+b*11U)&255U);
                pixels[i].a=static_cast<std::uint8_t>((b%3U)==0U?255U:((x*47U+y*41U+b)&255U));
            }
            std::array<std::uint8_t, 16> block{}, repeat{};
            (void)basist::bc7f::fast_pack_bc7_auto_rgba(block.data(),pixels.data(),flags);
            (void)basist::bc7f::fast_pack_bc7_auto_rgba(repeat.data(),pixels.data(),flags);
            if (block!=repeat) throw std::runtime_error("bc7f generated non-deterministic block");
            if (block[0]==0U && block[1]==0U) throw std::runtime_error("bc7f invalid mode prefix");
            accumulatedError+=basist::bc7f::calc_sse(block.data(),pixels.data());
            for (auto v:block) checksum=(checksum^v)*1099511628211ULL;
        }
        const auto us=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
        const double mse=static_cast<double>(accumulatedError)/(static_cast<double>(kBlocks)*16.0*4.0);
        const double psnr=mse>0.0?10.0*std::log10((255.0*255.0)/mse):99.0;
        std::cout<<"{\"encoder\":\"bc7f\",\"mode\":\""
                 <<(flags==basist::bc7f::cPackBC7FlagDefault?"analytical":"partially_analytical")
                 <<"\",\"blocks\":"<<kBlocks<<",\"ns_per_block\":"<<(us*1000.0/kBlocks)
                 <<",\"psnr_db\":"<<psnr<<",\"checksum\":"<<checksum<<"}\n";
    }
    return 0;
}
