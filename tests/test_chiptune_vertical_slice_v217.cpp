#include "dve/chiptune_vertical_slice.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef DVE_SOURCE_DIR
#define DVE_SOURCE_DIR "."
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<dve::gameplay::ChiptuneSliceInput> make_replay() {
    std::vector<dve::gameplay::ChiptuneSliceInput> replay(260U);
    for (auto& input : replay) input.moveX = 1.0F;
    replay[45].firePressed = true;
    replay[74].jumpPressed = true;
    for (std::size_t frame = 74U; frame < 102U; ++frame) replay[frame].jumpHeld = true;
    replay[130].firePressed = true;
    return replay;
}

bool has_event(const dve::gameplay::ChiptuneVerticalSlice& slice,
               dve::gameplay::ChiptuneSliceEventType type) {
    return std::any_of(slice.events().begin(), slice.events().end(),
                       [type](const auto& event) { return event.type == type; });
}

} // namespace

int main() {
    using namespace dve;
    using namespace dve::audio;
    using namespace dve::gameplay;
    try {
        AudioMixer mixer(48000U);
        ChiptuneVerticalSlice slice(mixer);
        std::string error;
        require(slice.initialize(DVE_SOURCE_DIR, {}, &error), error);
        require(slice.map().name == "v213_original_level", "original tile level was not loaded");
        require(static_cast<bool>(slice.music_source()), "music did not start on initialization");
        require(has_event(slice, ChiptuneSliceEventType::MusicStarted), "music-start event is missing");

        std::vector<float> output(4096U * 2U);
        mixer.render(output);
        require(std::any_of(output.begin(), output.end(), [](float value) { return std::abs(value) > 1.0e-5F; }),
                "vertical-slice music produced silence");
        require(mixer.meters().buses[audio_bus_index(AudioBusId::Music)].peakLeft > 0.0F,
                "music was not routed through the Music bus");

        const auto replay = make_replay();
        slice.run_replay(replay, 1.0F / 60.0F);
        require(has_event(slice, ChiptuneSliceEventType::Shot), "projectile input did not emit a shot");
        require(has_event(slice, ChiptuneSliceEventType::Jumped), "jump input did not emit a jump");
        require(!slice.hud_text().empty(), "HUD was not updated");
        require(slice.state().player.min.x > 250.0F, "player did not traverse the original level");
        require(has_event(slice, ChiptuneSliceEventType::EnemyDefeated) && !slice.state().enemyAlive,
                "projectile did not defeat the original patrol enemy");
        require(has_event(slice, ChiptuneSliceEventType::Collected) && slice.state().tokens == 1,
                "original collectible was not acquired");
        require(has_event(slice, ChiptuneSliceEventType::CheckpointActivated) &&
                    slice.state().checkpointActive,
                "original checkpoint was not activated");
        require(has_event(slice, ChiptuneSliceEventType::Finished) && slice.state().finished,
                "original playable vertical slice did not complete");

        std::fill(output.begin(), output.end(), 0.0F);
        mixer.render(output);
        require(mixer.meters().buses[audio_bus_index(AudioBusId::Effects)].peakLeft > 0.0F,
                "tracker SFX were not routed through the Effects bus");

        const auto saved = slice.capture_snapshot();
        slice.save_game();
        ChiptuneSliceInput disturb;
        disturb.moveX = -1.0F;
        for (int i = 0; i < 20; ++i) slice.step(disturb, 1.0F / 60.0F);
        require(slice.load_game(), "save/load workflow failed");
        require(slice.deterministic_hash() == saved.stateHash, "loaded game did not restore the saved state");

        AudioMixer mixer2(48000U);
        ChiptuneVerticalSlice replayA(mixer2);
        require(replayA.initialize(DVE_SOURCE_DIR, {}, &error), error);
        AudioMixer mixer3(48000U);
        ChiptuneVerticalSlice replayB(mixer3);
        require(replayB.initialize(DVE_SOURCE_DIR, {}, &error), error);
        replayA.run_replay(replay, 1.0F / 60.0F);
        replayB.run_replay(replay, 1.0F / 60.0F);
        require(replayA.deterministic_hash() == replayB.deterministic_hash(),
                "vertical-slice replay is not deterministic");

        const int deaths = slice.state().deaths;
        slice.restart_from_checkpoint();
        require(slice.state().deaths == deaths + 1 && slice.state().health == 3,
                "checkpoint restart did not reset player state");
        require(has_event(slice, ChiptuneSliceEventType::Respawned), "respawn event is missing");

        std::cout << "dve_chiptune_vertical_slice_v217_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_chiptune_vertical_slice_v217_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
