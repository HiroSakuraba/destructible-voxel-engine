#include "dve/sprite_vertical_slice_presentation.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

namespace {
using namespace dve;
using namespace dve::gameplay;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<ChiptuneSliceInput> replay() {
    std::vector<ChiptuneSliceInput> result(260U);
    for (auto& input : result) input.moveX = 1.0F;
    result[45U].firePressed = true;
    result[74U].jumpPressed = true;
    for (std::size_t frame = 74U; frame < 102U; ++frame) result[frame].jumpHeld = true;
    result[130U].firePressed = true;
    return result;
}

std::uint64_t run_once() {
    audio::AudioMixer mixer(48000U);
    ChiptuneVerticalSlice slice(mixer);
    SpriteVerticalSlicePresentation presentation;
    std::string error;
    require(slice.initialize(DVE_SOURCE_DIR, {}, &error), error);
    require(presentation.initialize(DVE_SOURCE_DIR, {}, &error), error);
    const auto inputs = replay();
    bool sawParticle{};
    bool sawProjectile{};
    for (const auto& input : inputs) {
        slice.step(input, 1.0F / 60.0F);
        presentation.sync(slice, 1.0F / 60.0F);
        const auto frame = presentation.build_frame(slice);
        sawParticle = sawParticle || frame.particles.stats.particles > 0U;
        for (const auto& item : frame.sprites.items) sawProjectile = sawProjectile || item.owner == 1003U;
    }
    const auto finalFrame = presentation.build_frame(slice);
    require(slice.state().finished, "presented vertical slice did not finish");
    require(finalFrame.sprites.items.size() >= 3U, "real sprite submissions are missing");
    require(!finalFrame.tiles.empty(), "native tile submissions are missing");
    require(!finalFrame.hudText.empty() && finalFrame.hudText.find("COMPLETE") != std::string::npos,
            "HUD presentation did not reach completion");
    require(sawParticle && sawProjectile, "event-driven particles or projectile presentation did not execute");
    require(!presentation.sprites().build_gameplay_debug_packets().empty(),
            "authored hitbox/socket tracks are not available in play presentation");
    return presentation.presentation_hash(slice) ^ slice.deterministic_hash();
}

} // namespace

int main() {
    try {
        const std::uint64_t first = run_once();
        const std::uint64_t second = run_once();
        require(first != 0U && first == second, "vertical-slice presentation is not deterministic");
        std::cout << "v2.18 sprite vertical slice presentation passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
