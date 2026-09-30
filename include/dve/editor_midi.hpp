#pragma once

// Editor-side MIDI setup shared by the SDL desktop editor and the X11 native editor:
// the `midi.*` settings, the input and output port-picker choices, and the host helper that
// creates the native (RtMidi) backends and attaches a MidiInputSession to the controller's
// synthesizer and a MidiOutputSession to its MIDI out.

#include "dve/audio/midi_input_session.hpp"
#include "dve/audio/midi_output_session.hpp"
#include "dve/editor_settings.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace dve::editor {

class NativeEditorController;

inline constexpr std::string_view kMidiInputPortSettingId = "midi.input_port";
inline constexpr std::string_view kMidiOutputPortSettingId = "midi.output_port";
inline constexpr std::string_view kMidiInputChannelSettingId = "midi.input_channel";
inline constexpr std::string_view kMidiDrumChannelSettingId = "midi.drum_channel";

// Channel filter from `midi.input_channel` ("omni" / "1".."16") and `midi.drum_channel`
// ("play" / "ignore" / "follow").
[[nodiscard]] audio::MidiChannelFilter midi_channel_filter_from_settings(const EditorSettingsRegistry& settings);
[[nodiscard]] std::string midi_input_port_from_settings(const EditorSettingsRegistry& settings);

// Picker entries for `midi.input_port`: Auto, None, every present port, and the saved port when
// it is currently unplugged. Labels carry the live status ("Auto: MPK mini 3 (connected)").
[[nodiscard]] std::vector<SettingChoice> midi_input_port_choices(const audio::MidiInputStatus& status,
                                                                std::string_view saved);
// Output port setting and picker entries for `midi.output_port`: Auto (first port), None, every
// present output port, and the saved port when it is currently unplugged.
[[nodiscard]] std::string midi_output_port_from_settings(const EditorSettingsRegistry& settings);
[[nodiscard]] std::vector<SettingChoice> midi_output_port_choices(const audio::MidiOutputStatus& status,
                                                                 std::string_view saved);
// Short text for the synth header button: "MIDI: MPK mini 3", "MIDI: off", ...
[[nodiscard]] std::string midi_input_button_label(const audio::MidiInputStatus& status);

struct EditorMidiStartResult {
    bool nativeBackend{};  // RtMidi compiled in and created
    std::string backendName; // "RtMidi" or "none"
    std::string error;
};

// Host helper: creates the native backends (if compiled), attaches the input and output sessions
// (worker threads poll ports ~every 1.5 s) and applies the saved `midi.*` settings, including the
// `midi.output_port` choice for the synth's MIDI out. Without RtMidi the controller reports
// "MIDI unavailable".
EditorMidiStartResult start_editor_midi(NativeEditorController& controller);

} // namespace dve::editor
