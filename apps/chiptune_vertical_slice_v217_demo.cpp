#include "dve/chiptune_vertical_slice.hpp"

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
    std::string error;
    if (!slice.initialize(DVE_SOURCE_DIR, {}, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::vector<ChiptuneSliceInput> replay(260U);
    for (auto& input : replay) input.moveX = 1.0F;
    replay[45].firePressed = true;
    replay[74].jumpPressed = true;
    for (std::size_t frame = 74U; frame < 102U; ++frame) replay[frame].jumpHeld = true;
    replay[130].firePressed = true;
    slice.run_replay(replay, 1.0F / 60.0F);

    const auto& state = slice.state();
    std::cout << "{\n"
              << "  \"level\": \"" << slice.map().name << "\",\n"
              << "  \"frame\": " << state.frame << ",\n"
              << "  \"player_x\": " << state.player.min.x << ",\n"
              << "  \"player_y\": " << state.player.min.y << ",\n"
              << "  \"health\": " << state.health << ",\n"
              << "  \"tokens\": " << state.tokens << ",\n"
              << "  \"enemy_alive\": " << (state.enemyAlive ? "true" : "false") << ",\n"
              << "  \"checkpoint_active\": " << (state.checkpointActive ? "true" : "false") << ",\n"
              << "  \"finished\": " << (state.finished ? "true" : "false") << ",\n"
              << "  \"state_hash\": " << slice.deterministic_hash() << ",\n"
              << "  \"hud\": \"" << slice.hud_text() << "\",\n"
              << "  \"events\": [\n";
    for (std::size_t index = 0; index < slice.events().size(); ++index) {
        const auto& event = slice.events()[index];
        std::cout << "    {\"frame\": " << event.frame
                  << ", \"type\": \"" << chiptune_slice_event_name(event.type)
                  << "\", \"detail\": \"" << event.detail << "\"}"
                  << (index + 1U == slice.events().size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
    return 0;
}
