#include "dve/audio/chiptune_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

using namespace dve::audio;

namespace {
int failures{};
void require(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void test_pattern_order_note_entry_and_history() {
    ChiptuneAuthoringSession session;
    const auto initialHash = session.song().content_hash();
    std::string error;
    require(session.add_pattern(32U, std::nullopt, &error), error.c_str());
    require(session.insert_order(1U, 1U, &error), error.c_str());
    require(session.set_cursor({1U, 0U, 0U, ChipTrackerField::Note}), "cursor rejected");
    require(session.enter_tracker_key("z", false, 1U, &error), error.c_str());
    require(session.song().order.size() == 2U, "order entry missing");
    const auto pattern = session.song().order[1U];
    const auto& cell = session.song().patterns[pattern].at(0U, 0U, session.song().channelCount);
    require(chip_note_to_midi(cell.note) == 60, "tracker keyboard did not enter C-4");
    require(cell.instrument == 1U, "selected instrument was not inserted");
    require(session.cursor().row == 1U, "note entry did not advance row");
    require(session.can_undo(), "editing did not create undo state");
    require(session.undo(&error), error.c_str());
    require(session.song().patterns[pattern].at(0U, 0U, session.song().channelCount).note == kChipNoteNone,
            "undo did not remove entered note");
    require(session.redo(&error), error.c_str());
    require(chip_note_to_midi(session.song().patterns[pattern].at(0U, 0U, session.song().channelCount).note) == 60,
            "redo did not restore note");
    require(session.song().content_hash() != initialHash, "edit did not change content hash");
}

void test_selection_copy_paste_transpose_and_clear() {
    ChiptuneAuthoringSession session;
    std::string error;
    require(session.set_cursor({0U, 0U, 0U, ChipTrackerField::Note}), "cursor rejected");
    require(session.enter_midi_note(60, false, 0U, &error), error.c_str());
    require(session.set_cursor({0U, 1U, 1U, ChipTrackerField::Note}), "cursor rejected");
    require(session.enter_midi_note(64, false, 0U, &error), error.c_str());
    ChipTrackerSelection selection;
    selection.anchor = {0U, 0U, 0U, ChipTrackerField::Note};
    selection.cursor = {0U, 1U, 1U, ChipTrackerField::EffectParameter};
    session.set_selection(selection);
    require(session.copy_selection(&error), error.c_str());
    require(session.clipboard().rows == 2U && session.clipboard().channels == 2U,
            "clipboard dimensions are wrong");
    require(session.set_cursor({0U, 4U, 2U, ChipTrackerField::Note}), "cursor rejected");
    require(session.paste_at_cursor(false, &error), error.c_str());
    const auto pattern = session.song().order[0];
    require(chip_note_to_midi(session.song().patterns[pattern].at(4U, 2U, session.song().channelCount).note) == 60,
            "paste lost first note");
    require(chip_note_to_midi(session.song().patterns[pattern].at(5U, 3U, session.song().channelCount).note) == 64,
            "paste lost second note");
    selection.anchor = {0U, 4U, 2U, ChipTrackerField::Note};
    selection.cursor = {0U, 5U, 3U, ChipTrackerField::EffectParameter};
    session.set_selection(selection);
    require(session.transpose_selection(12, &error), error.c_str());
    require(chip_note_to_midi(session.song().patterns[pattern].at(4U, 2U, session.song().channelCount).note) == 72,
            "transpose failed");
    require(session.cut_selection(&error), error.c_str());
    require(session.song().patterns[pattern].at(4U, 2U, session.song().channelCount).note == kChipNoteNone,
            "cut did not clear selection");
}

void test_effect_helpers_envelopes_and_wavetable() {
    ChiptuneAuthoringSession session;
    std::string error;
    require(session.set_cursor({0U, 0U, 0U, ChipTrackerField::Effect}), "cursor rejected");
    require(session.set_current_effect(ChipEffect::Arpeggio, {4, 7}, &error), error.c_str());
    const auto pattern = session.song().order[0];
    const auto& cell = session.song().patterns[pattern].at(0U, 0U, session.song().channelCount);
    require(cell.effect == ChipEffect::Arpeggio && cell.effectParam == 0x47U,
            "arpeggio helper encoded the wrong parameter");
    require(session.resize_envelope(ChipEnvelopeLane::Volume, 8U, 0.5F, &error), error.c_str());
    require(session.draw_envelope_point(ChipEnvelopeLane::Volume, 3U, 1.0F, &error), error.c_str());
    require(session.draw_envelope_point(ChipEnvelopeLane::Pitch, 2U, -12.0F, &error), error.c_str());
    require(session.set_envelope_loop(ChipEnvelopeLane::Volume, 2U, &error), error.c_str());
    require(session.generate_wavetable(ChipWavetableShape::Sawtooth, 32U, 0.5F, &error), error.c_str());
    require(session.remove_wavetable_dc(&error), error.c_str());
    require(session.normalize_wavetable(&error), error.c_str());
    const auto& instrument = session.song().instruments[session.selected_instrument()];
    require(instrument.volume.values.size() == 8U && instrument.volume.values[3] == 1.0F,
            "volume drawing failed");
    require(instrument.pitch.semitones.size() >= 3U && instrument.pitch.semitones[2] == -12.0F,
            "pitch envelope drawing failed");
    require(instrument.wave == ChipWave::Wavetable && instrument.wavetable.size() == 32U,
            "wavetable generation failed");
    float peak{};
    float mean{};
    for (float value : instrument.wavetable) { peak = std::max(peak, std::abs(value)); mean += value; }
    mean /= static_cast<float>(instrument.wavetable.size());
    require(std::abs(peak - 1.0F) < 1.0e-5F, "wavetable normalization failed");
    require(std::abs(mean) < 1.0e-5F, "wavetable DC removal failed");
}

void test_sfx_customization_preview_and_routing() {
    ChiptuneAuthoringSession session;
    ChipSfxRequest request;
    request.preset = ChipSfxPreset::Laser;
    request.durationSeconds = 0.2F;
    request.baseMidi = 84;
    request.gain = 0.55F;
    request.pan = -0.75F;
    session.set_sfx_request(request);
    std::string error;
    require(session.apply_sfx_request(&error), error.c_str());
    session.set_song_bus(AudioBusId::Music);
    session.set_instrument_bus(AudioBusId::UserInterface);
    require(session.song_bus() == AudioBusId::Music && session.instrument_bus() == AudioBusId::UserInterface,
            "bus routing state was not retained");
    const auto song = session.render_song_preview(0.3F, &error);
    require(error.empty() && !song.samples.empty(), "song preview failed");
    error.clear();
    const auto instrument = session.render_instrument_preview(72, 0.4F, &error);
    require(error.empty() && !instrument.samples.empty(), "instrument preview failed");
    AudioMixer mixer(48000U);
    const auto audition = session.audition_instrument(mixer, 72, 0.25F);
    require(static_cast<bool>(audition), audition.error.c_str());
    require(audition.bus == AudioBusId::UserInterface, "instrument audition used the wrong bus");
    std::vector<float> output(1024U * 2U);
    mixer.render(output);
    require(std::any_of(output.begin(), output.end(), [](float value) { return std::abs(value) > 1.0e-5F; }),
            "mixer audition produced silence");
    session.stop_audition(mixer, 0.0F);
    require(!mixer.chiptune_preview_active(), "live tracker preview did not stop");
    for (int previewIndex = 0; previewIndex < 96; ++previewIndex) {
        require(session.draw_envelope_point(ChipEnvelopeLane::Volume, 0U,
                    0.25F + static_cast<float>(previewIndex % 8) * 0.08F, &error), error.c_str());
        const auto repeated = session.audition_song(mixer, 0.2F, false);
        require(static_cast<bool>(repeated), repeated.error.c_str());
        mixer.render(output);
        session.stop_audition(mixer, 0.0F);
    }
}

void test_file_roundtrip() {
    ChiptuneAuthoringSession session;
    const auto root = std::filesystem::temp_directory_path() / "dve_chiptune_authoring_v217";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::string error;
    require(session.set_cursor({0U, 3U, 0U, ChipTrackerField::Note}), "cursor rejected");
    require(session.enter_midi_note(67, false, 0U, &error), error.c_str());
    const auto expected = session.song().content_hash();
    require(session.save_file(root / "song.dvechip", &error), error.c_str());
    require(!session.dirty(), "save did not clear dirty state");
    ChiptuneAuthoringSession loaded;
    require(loaded.load_file(root / "song.dvechip", &error), error.c_str());
    require(loaded.song().content_hash() == expected, "authoring file roundtrip changed song");
    std::filesystem::remove_all(root);
}
}

int main() {
    test_pattern_order_note_entry_and_history();
    test_selection_copy_paste_transpose_and_clear();
    test_effect_helpers_envelopes_and_wavetable();
    test_sfx_customization_preview_and_routing();
    test_file_roundtrip();
    if (failures == 0) { std::cout << "chiptune authoring v2.17: all tests passed\n"; return 0; }
    std::cerr << "chiptune authoring v2.17: " << failures << " failure(s)\n";
    return 1;
}
