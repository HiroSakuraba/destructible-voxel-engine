#include "dve/editor_midi.hpp"

#include "dve/editor_native.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

namespace dve::editor {
namespace {

std::string read_string(const EditorSettingsRegistry& settings, std::string_view id, std::string fallback) {
    const SettingValue value = settings.value(id);
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    return fallback;
}

} // namespace

audio::MidiChannelFilter midi_channel_filter_from_settings(const EditorSettingsRegistry& settings) {
    audio::MidiChannelFilter filter;
    const std::string channel = read_string(settings, kMidiInputChannelSettingId, "omni");
    int number = 0;
    const auto [end, ec] = std::from_chars(channel.data(), channel.data() + channel.size(), number);
    (void)end;
    filter.channel = ec == std::errc{} && number >= 1 && number <= 16 ? static_cast<std::uint8_t>(number) : 0U;
    const std::string drums = read_string(settings, kMidiDrumChannelSettingId, "play");
    filter.drums = drums == "ignore" ? audio::MidiDrumChannelMode::Ignore
                 : drums == "follow" ? audio::MidiDrumChannelMode::Follow
                                     : audio::MidiDrumChannelMode::Play;
    return filter;
}

std::string midi_input_port_from_settings(const EditorSettingsRegistry& settings) {
    return read_string(settings, kMidiInputPortSettingId, "");
}

std::vector<SettingChoice> midi_input_port_choices(const audio::MidiInputStatus& status, std::string_view saved) {
    using audio::MidiConnectionState;
    const bool connected = status.state == MidiConnectionState::Connected;
    std::vector<SettingChoice> choices;
    std::string autoLabel = "Auto";
    if (status.state == MidiConnectionState::Unavailable) autoLabel += " (MIDI unavailable)";
    else if (saved.empty()) autoLabel += connected ? ": " + audio::midi_port_display_name(status.connectedPort) + " (connected)"
                                                   : " (no device)";
    choices.push_back({"", std::move(autoLabel)});
    choices.push_back({std::string(audio::kMidiPortNone), "None (MIDI input off)"});
    bool savedListed = saved.empty() || saved == audio::kMidiPortNone;
    for (const std::string& port : status.ports) {
        const std::string value = audio::normalize_midi_port_name(port);
        if (std::any_of(choices.begin(), choices.end(), [&](const SettingChoice& c) { return c.value == value; })) continue;
        std::string label = value;
        audio::MidiPortDescriptor descriptor;
        descriptor.name = port;
        if (audio::is_midi_through_or_virtual_port(descriptor)) label += " [through/virtual]";
        if (connected && audio::normalize_midi_port_name(status.connectedPort) == value) label += " (connected)";
        if (!saved.empty() && audio::normalize_midi_port_name(saved) == value) savedListed = true;
        choices.push_back({value, std::move(label)});
    }
    if (!savedListed) choices.push_back({std::string(saved), std::string(saved) + " (disconnected)"});
    return choices;
}

std::string midi_output_port_from_settings(const EditorSettingsRegistry& settings) {
    return read_string(settings, kMidiOutputPortSettingId, "");
}

std::vector<SettingChoice> midi_output_port_choices(const audio::MidiOutputStatus& status, std::string_view saved) {
    using audio::MidiConnectionState;
    const bool connected = status.state == MidiConnectionState::Connected;
    std::vector<SettingChoice> choices;
    std::string autoLabel = "Auto";
    if (status.state == MidiConnectionState::Unavailable) autoLabel += " (MIDI unavailable)";
    else if (saved.empty()) autoLabel += connected ? ": " + audio::midi_port_display_name(status.connectedPort) + " (connected)"
                                                   : " (no output)";
    choices.push_back({"", std::move(autoLabel)});
    choices.push_back({std::string(audio::kMidiPortNone), "None (MIDI output off)"});
    bool savedListed = saved.empty() || saved == audio::kMidiPortNone;
    for (const std::string& port : status.ports) {
        const std::string value = audio::normalize_midi_port_name(port);
        if (std::any_of(choices.begin(), choices.end(), [&](const SettingChoice& c) { return c.value == value; })) continue;
        std::string label = value;
        audio::MidiPortDescriptor descriptor;
        descriptor.name = port;
        if (audio::is_midi_through_or_virtual_port(descriptor)) label += " [through/virtual]";
        if (connected && audio::normalize_midi_port_name(status.connectedPort) == value) label += " (connected)";
        if (!saved.empty() && audio::normalize_midi_port_name(saved) == value) savedListed = true;
        choices.push_back({value, std::move(label)});
    }
    if (!savedListed) choices.push_back({std::string(saved), std::string(saved) + " (disconnected)"});
    return choices;
}

std::string midi_input_button_label(const audio::MidiInputStatus& status) {
    using audio::MidiConnectionState;
    switch (status.state) {
        case MidiConnectionState::Unavailable: return "MIDI IN: n/a";
        case MidiConnectionState::Disabled: return "MIDI IN: off";
        case MidiConnectionState::Connected: return "MIDI IN: " + audio::midi_port_display_name(status.connectedPort);
        case MidiConnectionState::Disconnected:
            return "MIDI IN: " + audio::midi_port_display_name(status.requestedPort) + " (unplugged)";
        case MidiConnectionState::NoDevice:
            return status.lastConnectedPort.empty() ? std::string("MIDI IN: no device")
                : "MIDI IN: " + audio::midi_port_display_name(status.lastConnectedPort) + " (unplugged)";
        case MidiConnectionState::Error: return "MIDI IN: error";
    }
    return "MIDI IN";
}

EditorMidiStartResult start_editor_midi(NativeEditorController& controller) {
    EditorMidiStartResult result;
    std::string error;
    auto input = audio::make_native_midi_backend(&error);
    result.nativeBackend = input != nullptr;
    result.backendName = input ? std::string(input->backend_name()) : std::string("none");
    result.error = error;
    controller.attach_midi_input(std::move(input));
    if (result.nativeBackend) {
        // Synth MIDI out (MIDI thru / arpeggiator output): the `midi.output_port` choice. Auto
        // keeps the previous behaviour, the first output port (on Linux usually "Midi Through").
        std::string outputError;
        controller.attach_midi_output(audio::make_native_midi_backend(&outputError));
    } else {
        controller.attach_midi_output(nullptr);
    }
    return result;
}

// ---- NativeEditorController MIDI members ----

void NativeEditorController::attach_midi_input(std::unique_ptr<audio::IMidiBackend> backend,
                                               audio::MidiInputSession::Options options) {
    midiInput_.reset();  // closes (and releases notes of) a previous session first
    audio::Synthesizer* synth = &audioMixer_.synthesizer();
    midiInput_ = std::make_unique<audio::MidiInputSession>(
        std::move(backend), [synth,wake=wakeCallback_](const audio::MidiMessage& message) { (void)synth->post_midi(message); if(wake) wake(); }, options);
    midiInput_->set_channel_filter(midi_channel_filter_from_settings(workspace_.settings()));
    midiInput_->set_requested_port(midi_input_port());
    midiStatusGeneration_ = ~std::uint64_t{0};
    refresh_midi_status();
}

void NativeEditorController::attach_midi_output(std::unique_ptr<audio::IMidiBackend> backend,
                                                audio::MidiOutputSession::Options options) {
    midiOutput_.reset(); // releases held notes on the previous port first
    midiOutput_ = std::make_unique<audio::MidiOutputSession>(std::move(backend), options);
    midiOutput_->set_requested_port(midi_output_port());
    midiOutputStatusGeneration_ = ~std::uint64_t{0};
    refresh_midi_status();
}

std::string NativeEditorController::midi_output_port() const {
    return midi_output_port_from_settings(workspace_.settings());
}

bool NativeEditorController::set_midi_output_port(std::string name) {
    std::string error;
    (void)workspace_.settings().clear(SettingScope::Session, kMidiOutputPortSettingId);
    if (!workspace_.settings().set(SettingScope::User, kMidiOutputPortSettingId, name, &error)) {
        set_status(error, true);
        return false;
    }
    if (midiOutput_) midiOutput_->set_requested_port(midi_output_port());
    refresh_midi_status();
    const std::string shown = name.empty() ? std::string("Auto") : name == audio::kMidiPortNone ? std::string("None") : name;
    std::string message = "MIDI output: " + shown;
    if (midi_output_port() != name) message += " (Project settings override the User value)";
    if (!save_user_settings(&error)) {
        set_status(message + "; save failed: " + error, true);
        return false;
    }
    set_status(std::move(message));
    return true;
}

bool NativeEditorController::cycle_midi_output_port(int direction) {
    const std::string current = midi_output_port();
    const auto choices = midi_output_port_choices(midiOutputStatus_, current);
    if (choices.empty()) return false;
    const auto it = std::find_if(choices.begin(), choices.end(), [&](const SettingChoice& c) {
        return c.value == current || (!current.empty() && current != audio::kMidiPortNone &&
                                      c.value == audio::normalize_midi_port_name(current));
    });
    const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(choices.size());
    std::ptrdiff_t index = it == choices.end() ? 0 : std::distance(choices.begin(), it);
    index = (index + (direction >= 0 ? 1 : -1) + count) % count;
    return set_midi_output_port(choices[static_cast<std::size_t>(index)].value);
}

std::string NativeEditorController::midi_input_port() const {
    return midi_input_port_from_settings(workspace_.settings());
}

bool NativeEditorController::set_midi_input_port(std::string name) {
    std::string error;
    (void)workspace_.settings().clear(SettingScope::Session, kMidiInputPortSettingId);
    if (!workspace_.settings().set(SettingScope::User, kMidiInputPortSettingId, name, &error)) {
        set_status(error, true);
        return false;
    }
    if (midiInput_) midiInput_->set_requested_port(midi_input_port());
    refresh_midi_status();
    const std::string shown = name.empty() ? std::string("Auto") : name == audio::kMidiPortNone ? std::string("None") : name;
    std::string message = "MIDI input: " + shown;
    if (midi_input_port() != name) message += " (Project settings override the User value)";
    if (!save_user_settings(&error)) {
        set_status(message + "; save failed: " + error, true);
        return false;
    }
    set_status(std::move(message));
    return true;
}

bool NativeEditorController::cycle_midi_input_port(int direction) {
    const std::string current = midi_input_port();
    const auto choices = midi_input_port_choices(midiStatus_, current);
    if (choices.empty()) return false;
    const auto it = std::find_if(choices.begin(), choices.end(), [&](const SettingChoice& c) {
        return c.value == current || (!current.empty() && current != audio::kMidiPortNone &&
                                      c.value == audio::normalize_midi_port_name(current));
    });
    const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(choices.size());
    std::ptrdiff_t index = it == choices.end() ? 0 : std::distance(choices.begin(), it);
    index = (index + (direction >= 0 ? 1 : -1) + count) % count;
    return set_midi_input_port(choices[static_cast<std::size_t>(index)].value);
}

void NativeEditorController::refresh_midi_status() {
    bool changed = false;
    if (midiInput_) {
        const std::uint64_t generation = midiInput_->status_generation();
        if (generation != midiStatusGeneration_) {
            const audio::MidiConnectionState previous = midiStatus_.state;
            const bool first = midiStatusGeneration_ == ~std::uint64_t{0};
            midiStatus_ = midiInput_->status();
            midiStatusGeneration_ = generation;
            changed = true;
            if (!first && previous != midiStatus_.state &&
                (midiStatus_.state == audio::MidiConnectionState::Connected ||
                 previous == audio::MidiConnectionState::Connected))
                set_status("MIDI " + midiStatus_.summary());
        }
    } else if (midiStatusGeneration_ == ~std::uint64_t{0}) {
        midiStatus_ = {};
        midiStatusGeneration_ = 0;
        changed = true;
    }
    const std::string saved = midi_input_port();
    if (changed || saved != midiChoicesPort_ || !settingsPanel_.dynamicChoices.contains(kMidiInputPortSettingId)) {
        midiChoicesPort_ = saved;
        settingsPanel_.dynamicChoices[std::string(kMidiInputPortSettingId)] = midi_input_port_choices(midiStatus_, saved);
        synthPanel_.set_midi_input_label(midi_input_button_label(midiStatus_),
                                         midiStatus_.state == audio::MidiConnectionState::Connected);
    }

    bool outputChanged = false;
    if (midiOutput_) {
        const std::uint64_t generation = midiOutput_->status_generation();
        if (generation != midiOutputStatusGeneration_) {
            const audio::MidiConnectionState previous = midiOutputStatus_.state;
            const bool first = midiOutputStatusGeneration_ == ~std::uint64_t{0};
            midiOutputStatus_ = midiOutput_->status();
            midiOutputStatusGeneration_ = generation;
            outputChanged = true;
            if (!first && previous != midiOutputStatus_.state &&
                (midiOutputStatus_.state == audio::MidiConnectionState::Connected ||
                 previous == audio::MidiConnectionState::Connected))
                set_status("MIDI out " + midiOutputStatus_.summary());
        }
    } else if (midiOutputStatusGeneration_ == ~std::uint64_t{0}) {
        midiOutputStatus_ = {};
        midiOutputStatusGeneration_ = 0;
        outputChanged = true;
    }
    const std::string savedOutput = midi_output_port();
    if (outputChanged || savedOutput != midiOutputChoicesPort_ ||
        !settingsPanel_.dynamicChoices.contains(kMidiOutputPortSettingId)) {
        midiOutputChoicesPort_ = savedOutput;
        settingsPanel_.dynamicChoices[std::string(kMidiOutputPortSettingId)] =
            midi_output_port_choices(midiOutputStatus_, savedOutput);
    }
}

} // namespace dve::editor
