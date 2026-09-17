#include "dve/sprite_vertical_slice_presentation.hpp"

#include <iostream>
#include <string>
#include <vector>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

int main() {
    using namespace dve;
    using namespace dve::gameplay;
    audio::AudioMixer mixer(48000U);
    ChiptuneVerticalSlice slice(mixer);
    SpriteVerticalSlicePresentation presentation;
    std::string error;
    if (!slice.initialize(DVE_SOURCE_DIR, {}, &error) ||
        !presentation.initialize(DVE_SOURCE_DIR, {}, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::vector<ChiptuneSliceInput> replay(260U);
    for (auto& input : replay) input.moveX = 1.0F;
    replay[45U].firePressed = true;
    replay[74U].jumpPressed = true;
    for (std::size_t frame = 74U; frame < 102U; ++frame) replay[frame].jumpHeld = true;
    replay[130U].firePressed = true;
    std::uint64_t maximumParticles{};
    for (const auto& input : replay) {
        slice.step(input, 1.0F / 60.0F);
        presentation.sync(slice, 1.0F / 60.0F);
        maximumParticles = std::max(maximumParticles,
            presentation.build_frame(slice).particles.stats.particles);
    }
    const auto frame = presentation.build_frame(slice);
    std::cout << "{\n"
              << "  \"level\": \"" << slice.map().name << "\",\n"
              << "  \"finished\": " << (slice.state().finished ? "true" : "false") << ",\n"
              << "  \"sprite_submissions\": " << frame.sprites.items.size() << ",\n"
              << "  \"sprite_batches\": " << frame.sprites.batches.size() << ",\n"
              << "  \"tile_submissions\": " << frame.tiles.size() << ",\n"
              << "  \"maximum_particles\": " << maximumParticles << ",\n"
              << "  \"state_hash\": " << slice.deterministic_hash() << ",\n"
              << "  \"presentation_hash\": " << presentation.presentation_hash(slice) << ",\n"
              << "  \"hud\": \"" << frame.hudText << "\"\n"
              << "}\n";
    return slice.state().finished ? 0 : 2;
}
