#include "dve/audio/ambisonics.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    using namespace dve::audio;
    require(ambisonic_channel_count(0) == 1 && ambisonic_channel_count(1) == 4 &&
                ambisonic_channel_count(2) == 9 && ambisonic_channel_count(3) == 0,
            "Ambisonic channel count contract mismatch");
    AmbisonicFieldProcessor processor(2);
    std::array<float, 32> mono{};
    mono.fill(0.25F);
    std::array<float, 32 * 9> field{};
    require(processor.encode_add(mono, field, {1.0F, 0.0F, 0.0F}),
            "rightward Ambisonic encode failed");
    std::array<float, 64> stereo{};
    require(processor.decode_stereo(field, stereo), "Ambisonic stereo decode failed");
    float left{}, right{};
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        left += std::abs(stereo[frame * 2U]);
        right += std::abs(stereo[frame * 2U + 1U]);
    }
    require(right > left, "rightward sound did not decode toward the right speaker");

    field.fill(0.0F);
    require(processor.encode_add(mono, field, {0.0F, 0.0F, 1.0F}),
            "forward Ambisonic encode failed");
    require(processor.decode_stereo(field, stereo), "forward decode failed");
    left = right = 0.0F;
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        left += std::abs(stereo[frame * 2U]);
        right += std::abs(stereo[frame * 2U + 1U]);
    }
    require(std::abs(left - right) < 1.0e-4F,
            "front-center sound did not decode symmetrically");
    std::cout << "DVE Ambisonic field tests passed\n";
}
