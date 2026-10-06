// Editor MIDI setup: the `midi.*` settings (port picker by name with live status, channel filter,
// channel 10 pads), User-scope persistence and reload, the synth header MIDI input button, status
// labels on unplug / replug, the MIDI output port picker (Auto, None, saved by name, hotplug,
// synth MIDI out reaching the chosen port), the shared start helper without a native backend, the chiptune
// "Keys N" button, and the Settings title no longer sitting under the scope tabs.
#include "dve/editor_midi.hpp"
#include "dve/editor_native.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool intersects(const UiRect& a, const UiRect& b) {
    return a.width > 0 && b.width > 0 && a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}
void click(NativeEditorController& controller, UiRect rect) {
    controller.pointer_down(PointerButton::Primary, rect.x + rect.width / 2, rect.y + rect.height / 2);
    controller.pointer_up(PointerButton::Primary, rect.x + rect.width / 2, rect.y + rect.height / 2);
}
std::string file_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

const std::string kThrough = "Midi Through:Midi Through Port-0 14:0";
const std::string kMpk = "MPK mini 3:MPK mini 3 MIDI 1 24:0";
const std::string kMpkReplugged = "MPK mini 3:MPK mini 3 MIDI 1 28:0";
const std::string kMpkSaved = "MPK mini 3:MPK mini 3 MIDI 1";

audio::FakeMidiPortBackend* attach_fake(NativeEditorController& controller, std::vector<std::string> ports) {
    auto backend = std::make_unique<audio::FakeMidiPortBackend>(std::move(ports));
    audio::FakeMidiPortBackend* raw = backend.get();
    controller.attach_midi_input(std::move(backend), {std::chrono::milliseconds(10), false});
    return raw;
}
void poll(NativeEditorController& controller) {
    controller.midi_input_session()->poll_now();
    controller.refresh_midi_status();
}
std::vector<std::string> choice_values(const NativeEditorController& controller) {
    std::vector<std::string> values;
    const auto it = controller.settings_panel().dynamicChoices.find(kMidiInputPortSettingId);
    if (it != controller.settings_panel().dynamicChoices.end())
        for (const SettingChoice& choice : it->second) values.push_back(choice.value);
    return values;
}
std::string choice_label(const NativeEditorController& controller, const std::string& value) {
    const auto it = controller.settings_panel().dynamicChoices.find(kMidiInputPortSettingId);
    if (it == controller.settings_panel().dynamicChoices.end()) return {};
    for (const SettingChoice& choice : it->second) if (choice.value == value) return choice.label;
    return {};
}

void test_picker_persistence_and_header(const std::filesystem::path& settingsFile) {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.configure_user_settings(settingsFile);
    check(controller.midi_input_port().empty(), "default MIDI input is not Auto");
    audio::FakeMidiPortBackend* backend = attach_fake(controller, {kThrough, kMpk});
    poll(controller);
    check(controller.midi_input_status().state == audio::MidiConnectionState::Connected &&
          controller.midi_input_status().connectedPort == kMpk, "Auto did not pick the MPK mini over Midi Through");
    check(controller.midi_input_summary() == "MPK mini 3: connected", "summary: " + controller.midi_input_summary());
    const auto values = choice_values(controller);
    const std::vector<std::string> expected{"", "none", "Midi Through:Midi Through Port-0", kMpkSaved};
    check(values == expected, "port picker choices wrong (" + std::to_string(values.size()) + " entries)");
    check(choice_label(controller, "") == "Auto: MPK mini 3 (connected)", "Auto label: " + choice_label(controller, ""));
    check(choice_label(controller, "Midi Through:Midi Through Port-0").find("[through/virtual]") != std::string::npos,
          "Through port not marked");
    check(controller.synth_panel().midi_input_label() == "MIDI IN: MPK mini 3" &&
          controller.synth_panel().midi_input_connected(), "synth header label: " + controller.synth_panel().midi_input_label());

    // Synth header button cycles Auto -> None -> ports and saves by name.
    (void)controller.dispatch_action("window.toggle_synth");
    click(controller, controller.synth_panel().layout().midiInputButton);
    check(controller.midi_input_port() == "none", "header button did not select None");
    check(file_text(settingsFile).find("\"midi.input_port\" string \"none\"") != std::string::npos,
          "None was not saved to the User settings file");
    poll(controller);
    check(controller.midi_input_status().state == audio::MidiConnectionState::Disabled, "None did not disable input");
    check(controller.synth_panel().midi_input_label() == "MIDI IN: off", "label after None: " + controller.synth_panel().midi_input_label());
    click(controller, controller.synth_panel().layout().midiInputButton);
    check(controller.midi_input_port() == "Midi Through:Midi Through Port-0", "header button did not step to the next port");
    poll(controller);
    check(backend->open_port_name() == kThrough, "explicit Through choice not opened");

    // Settings > Audio > MIDI: left/right cycles the live port list; Apply saves by name.
    controller.open_settings(SettingScope::User, "Audio");
    const auto rows = controller.settings_rows();
    const auto row = std::find_if(rows.begin(), rows.end(), [](const SettingDefinition* d) { return d->id == kMidiInputPortSettingId; });
    check(row != rows.end(), "MIDI Input row missing from Settings > Audio");
    if (row != rows.end()) {
        check(controller.settings_panel().has_choices(**row), "MIDI Input row is not a picker");
        controller.settings_panel().selectedRow = static_cast<std::size_t>(row - rows.begin());
        controller.key_down("right", false, false, false);
        const auto staged = controller.settings_panel().stagedValues.find(kMidiInputPortSettingId);
        check(staged != controller.settings_panel().stagedValues.end() &&
              std::get<std::string>(staged->second) == kMpkSaved, "right arrow did not stage the MPK mini");
        check(controller.settings_panel().value_label(**row, SettingValue{std::string(kMpkSaved)}) == kMpkSaved,
              "picker label for a port");
    }
    std::string error;
    check(controller.settings_panel().stage(controller.workspace().settings(), kMidiInputChannelSettingId,
                                            std::string("2"), &error), "could not stage channel 2: " + error);
    check(controller.settings_panel().stage(controller.workspace().settings(), kMidiDrumChannelSettingId,
                                            std::string("ignore"), &error), "could not stage drum ignore: " + error);
    controller.close_settings(true);
    check(controller.midi_input_port() == kMpkSaved, "Apply did not store the port by name");
    const audio::MidiChannelFilter filter = controller.midi_input_session()->channel_filter();
    check(filter.channel == 2U && filter.drums == audio::MidiDrumChannelMode::Ignore, "channel filter not applied to the session");
    poll(controller);
    check(backend->open_port_name() == kMpk, "applied port not opened");
    const std::string saved = file_text(settingsFile);
    check(saved.find("\"midi.input_port\" string \"" + kMpkSaved + "\"") != std::string::npos, "port name not saved");
    check(saved.find("\"midi.input_channel\" enum \"2\"") != std::string::npos, "channel not saved");
    check(saved.find("\"midi.drum_channel\" enum \"ignore\"") != std::string::npos, "drum mode not saved");

    // Channel filter end-to-end: channel 2 plays, channel 1 and the pads do not.
    const std::uint64_t filteredBefore = controller.midi_input_session()->messages_filtered();
    (void)backend->inject(audio::MidiMessage::note_on(1, 60, 100));
    (void)backend->inject(audio::MidiMessage::note_on(0, 62, 100));
    (void)backend->inject(audio::MidiMessage::note_on(9, 36, 100));
    check(controller.midi_input_session()->messages_filtered() - filteredBefore == 2U, "filter did not drop channel 1 and the pads");

    // Unplug / replug with a new ALSA id: status and label follow, the saved port reconnects.
    backend->unplug(kMpk);
    poll(controller);
    check(controller.midi_input_summary() == "MPK mini 3: disconnected", "summary after unplug: " + controller.midi_input_summary());
    check(controller.synth_panel().midi_input_label() == "MIDI IN: MPK mini 3 (unplugged)", "label after unplug: " +
          controller.synth_panel().midi_input_label());
    check(choice_label(controller, kMpkSaved) == kMpkSaved + " (disconnected)", "picker keeps the unplugged saved port: " +
          choice_label(controller, kMpkSaved));
    backend->plug(kMpkReplugged);
    poll(controller);
    check(backend->open_port_name() == kMpkReplugged, "saved port did not reconnect after replug");
    check(controller.synth_panel().midi_input_connected(), "header not green after replug");
}

void test_reload(const std::filesystem::path& settingsFile) {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.configure_user_settings(settingsFile);
    check(controller.midi_input_port() == kMpkSaved, "saved port not loaded from User settings");
    audio::FakeMidiPortBackend* backend = attach_fake(controller, {kThrough, "Arturia KeyStep 37:Arturia KeyStep 37 MIDI 1 32:0", kMpkReplugged});
    poll(controller);
    check(backend->open_port_name() == kMpkReplugged, "startup did not auto-select the saved port");
    const audio::MidiChannelFilter filter = controller.midi_input_session()->channel_filter();
    check(filter.channel == 2U && filter.drums == audio::MidiDrumChannelMode::Ignore, "channel filter not restored");
}

std::vector<std::string> output_choice_values(const NativeEditorController& controller) {
    std::vector<std::string> values;
    const auto it = controller.settings_panel().dynamicChoices.find(kMidiOutputPortSettingId);
    if (it != controller.settings_panel().dynamicChoices.end())
        for (const SettingChoice& choice : it->second) values.push_back(choice.value);
    return values;
}
std::string output_choice_label(const NativeEditorController& controller, const std::string& value) {
    const auto it = controller.settings_panel().dynamicChoices.find(kMidiOutputPortSettingId);
    if (it == controller.settings_panel().dynamicChoices.end()) return {};
    for (const SettingChoice& choice : it->second) if (choice.value == value) return choice.label;
    return {};
}

const std::string kJdxi = "Roland JD-Xi:JD-Xi MIDI 1 20:0";
const std::string kJdxiReplugged = "Roland JD-Xi:JD-Xi MIDI 1 36:0";
const std::string kJdxiSaved = "Roland JD-Xi:JD-Xi MIDI 1";

void test_output_picker(const std::filesystem::path& settingsFile) {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.configure_user_settings(settingsFile);
    check(controller.midi_output_port().empty(), "default MIDI output is not Auto");
    auto owned = std::make_unique<audio::FakeMidiPortBackend>(std::vector<std::string>{}, std::vector<std::string>{kThrough, kJdxi});
    audio::FakeMidiPortBackend* backend = owned.get();
    controller.attach_midi_output(std::move(owned), {std::chrono::milliseconds(10), false});
    const auto poll_output = [&] {
        controller.midi_output_session()->poll_now();
        controller.refresh_midi_status();
    };
    poll_output();
    // Auto keeps the previous behaviour: the first output port.
    check(controller.midi_output_status().state == audio::MidiConnectionState::Connected &&
          backend->open_output_port_name() == kThrough, "Auto output did not open the first port");
    check(controller.midi_output_summary() == "Midi Through: connected", "output summary: " + controller.midi_output_summary());
    const std::vector<std::string> expected{"", "none", "Midi Through:Midi Through Port-0", kJdxiSaved};
    check(output_choice_values(controller) == expected, "output picker choices wrong");
    check(output_choice_label(controller, "") == "Auto: Midi Through (connected)", "output Auto label: " + output_choice_label(controller, ""));
    check(output_choice_label(controller, "none") == "None (MIDI output off)", "output None label");
    // The input picker is untouched by the output list.
    const auto inputValues = choice_values(controller);
    check(std::find(inputValues.begin(), inputValues.end(), kJdxiSaved) == inputValues.end(),
          "input picker picked up output ports");

    // update() drains the synth MIDI out into the connected session; sends reach the port.
    controller.update(0.0F);
    backend->clear_sent();
    check(controller.midi_output_session()->send(audio::MidiMessage::note_on(0, 61, 100)), "session send failed");
    auto sent = backend->sent_messages();
    check(sent.size() == 1U && sent[0].first == kThrough && sent[0].second.data1 == 61U, "message not delivered");

    // Settings > Audio > MIDI Output: left/right cycles the live list; Apply saves by name.
    controller.open_settings(SettingScope::User, "Audio");
    const auto rows = controller.settings_rows();
    const auto row = std::find_if(rows.begin(), rows.end(), [](const SettingDefinition* d) { return d->id == kMidiOutputPortSettingId; });
    check(row != rows.end(), "MIDI Output row missing from Settings > Audio");
    if (row != rows.end()) {
        check(controller.settings_panel().has_choices(**row), "MIDI Output row is not a picker");
        controller.settings_panel().selectedRow = static_cast<std::size_t>(row - rows.begin());
        controller.key_down("right", false, false, false); // Auto -> None
        controller.key_down("right", false, false, false); // None -> Midi Through
        controller.key_down("right", false, false, false); // -> JD-Xi
        const auto staged = controller.settings_panel().stagedValues.find(kMidiOutputPortSettingId);
        check(staged != controller.settings_panel().stagedValues.end() &&
              std::get<std::string>(staged->second) == kJdxiSaved, "right arrow did not stage the JD-Xi");
    }
    controller.close_settings(true);
    check(controller.midi_output_port() == kJdxiSaved, "Apply did not store the output port by name");
    check(file_text(settingsFile).find("\"midi.output_port\" string \"" + kJdxiSaved + "\"") != std::string::npos,
          "output port not saved to the User settings file");
    backend->clear_sent();
    poll_output();
    check(backend->open_output_port_name() == kJdxi, "applied output port not opened");
    std::size_t released = 0;
    for (const auto& [port, message] : backend->sent_messages())
        if (port == kThrough && message.type == audio::MidiMessageType::ControlChange && message.data1 == 123U) ++released;
    check(released == 16U, "switching ports did not release notes on the old port");
    check(output_choice_label(controller, kJdxiSaved) == kJdxiSaved + " (connected)", "connected label: " +
          output_choice_label(controller, kJdxiSaved));

    // Unplug / replug with a new ALSA id: picker keeps the saved port, reconnects by name.
    backend->unplug_output(kJdxi);
    poll_output();
    check(controller.midi_output_summary() == "Roland JD-Xi: disconnected", "output summary after unplug: " + controller.midi_output_summary());
    check(output_choice_label(controller, kJdxiSaved) == kJdxiSaved + " (disconnected)", "picker lost the unplugged output");
    backend->plug_output(kJdxiReplugged);
    poll_output();
    check(backend->open_output_port_name() == kJdxiReplugged, "saved output did not reconnect after replug");

    // cycle_midi_output_port walks Auto -> None -> ports like the input one.
    check(controller.cycle_midi_output_port(1) && controller.midi_output_port().empty(),
          "cycle from the JD-Xi did not wrap to Auto: " + controller.midi_output_port());
    check(controller.cycle_midi_output_port(1) && controller.midi_output_port() == "none",
          "cycle from Auto did not step to None: " + controller.midi_output_port());
    poll_output();
    check(controller.midi_output_status().state == audio::MidiConnectionState::Disabled && !backend->output_open(),
          "None did not close the output");
    check(controller.set_midi_output_port(kJdxiSaved), "restore saved output");
}

void test_output_reload(const std::filesystem::path& settingsFile) {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.configure_user_settings(settingsFile);
    check(controller.midi_output_port() == kJdxiSaved, "saved output port not loaded from User settings");
    auto owned = std::make_unique<audio::FakeMidiPortBackend>(std::vector<std::string>{},
                                                              std::vector<std::string>{kThrough, kJdxiReplugged});
    audio::FakeMidiPortBackend* backend = owned.get();
    controller.attach_midi_output(std::move(owned), {std::chrono::milliseconds(10), false});
    controller.midi_output_session()->poll_now();
    controller.refresh_midi_status();
    check(backend->open_output_port_name() == kJdxiReplugged, "startup did not open the saved output port");
}

void test_without_native_backend() {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.attach_midi_input(nullptr);
    controller.refresh_midi_status();
    check(controller.midi_input_status().state == audio::MidiConnectionState::Unavailable, "null backend not Unavailable");
    check(controller.synth_panel().midi_input_label() == "MIDI IN: n/a", "label without backend");
    check(choice_values(controller).size() == 2U, "picker without backend should offer Auto and None");
    controller.attach_midi_output(nullptr);
    controller.refresh_midi_status();
    check(controller.midi_output_status().state == audio::MidiConnectionState::Unavailable, "null output not Unavailable");
    check(output_choice_values(controller).size() == 2U, "output picker without backend should offer Auto and None");
    // The shared host helper works whether or not RtMidi was compiled in.
    const EditorMidiStartResult start = start_editor_midi(controller);
    check(start.backendName == (start.nativeBackend ? std::string("RtMidi") : std::string("none")), "start helper backend name");
    controller.update(0.0F);
    if (!start.nativeBackend)
        check(controller.midi_input_status().state == audio::MidiConnectionState::Unavailable,
              "no native backend should report Unavailable");
}

void test_chiptune_keys_button(const std::filesystem::path& settingsFile) {
    NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
    controller.configure_user_settings(settingsFile);
    (void)controller.dispatch_action("window.toggle_chiptune");
    check(controller.chiptune_panel().open(), "chiptune panel did not open");
    click(controller, controller.chiptune_panel().layout().tabs[2]);
    check(controller.chiptune_panel().page() == ChiptunePanelPage::Sfx, "SFX page did not open");
    const UiRect button = controller.chiptune_panel().layout().keyboardKeysButton;
    check(button.width > 0 && !intersects(button, controller.chiptune_panel().layout().auditionSfxButton) &&
          !intersects(button, controller.chiptune_panel().layout().pianoArea), "chiptune Keys button overlaps");
    click(controller, button);
    check(controller.keyboard_key_count() == 37, "chiptune Keys button did not step to 37 keys");
    check(controller.chiptune_panel().keyboard().key_count() == 37 && controller.synth_panel().keyboard().key_count() == 37,
          "key count not applied to both pianos");
    check(file_text(settingsFile).find("\"editor.keyboard_keys\" enum \"37\"") != std::string::npos, "key count not saved");
}

void test_settings_title() {
    for (auto [w, h] : std::vector<std::pair<int, int>>{{640, 480}, {800, 600}, {1024, 575}, {1280, 800}, {2560, 1440}}) {
        NativeEditorController controller{EditorWorkspace(make_native_editor_demo_document())};
        controller.resize(w, h);
        controller.open_settings(SettingScope::User, "Audio");
        const NativeSettingsModalLayout layout = controller.settings_modal_layout();
        const std::string tag = std::to_string(w) + "x" + std::to_string(h) + ": ";
        check(layout.title.width >= 120, tag + "settings title cell too narrow");
        for (const UiRect& tab : layout.scopeTabs) {
            check(!intersects(layout.title, tab), tag + "settings title under a scope tab");
            check(tab.x + tab.width <= layout.panel.x + layout.panel.width, tag + "scope tab off the modal");
        }
    }
}

} // namespace

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dve_editor_midi_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path settingsFile = dir / "editor_settings.txt";
    test_picker_persistence_and_header(settingsFile);
    test_reload(settingsFile);
    test_output_picker(settingsFile);
    test_output_reload(settingsFile);
    test_without_native_backend();
    test_chiptune_keys_button(settingsFile);
    test_settings_title();
    std::filesystem::remove_all(dir);
    if (g_failures == 0) std::printf("dve_editor_midi_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
