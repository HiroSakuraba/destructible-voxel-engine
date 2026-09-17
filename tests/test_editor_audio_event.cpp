#include "dve/editor_native.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    using namespace dve;
    using namespace dve::editor;
    NativeEditorController controller;
    const auto window = controller.workspace().menus().menu("Window");
    require(std::any_of(window.begin(), window.end(), [](const auto& action) {
        return action.id == "window.toggle_audio_event" && action.shortcut == "Ctrl+6";
    }), "Audio Event Graph is missing from the Window menu");
    controller.key_down("6", true, false, false);
    require(controller.audio_event_panel().open(), "Ctrl+6 did not open Audio Event Graph");
    require(controller.audio_event_panel().report().succeeded(), "default event graph did not compile");

    const auto before = controller.audio_event_panel().asset().graph.nodes.size();
    const auto add = controller.audio_event_panel().layout().addNoteButton;
    controller.pointer_down(PointerButton::Primary, add.x + 3, add.y + 3);
    require(controller.audio_event_panel().asset().graph.nodes.size() == before + 1,
            "Add Note did not mutate the graph");
    require(controller.audio_event_panel().report().succeeded(), "graph failed after Add Note");

    const auto audition = controller.audio_event_panel().layout().auditionButton;
    controller.pointer_down(PointerButton::Primary, audition.x + 3, audition.y + 3);
    std::array<float, 1024> audio{};
    controller.audio_mixer().render(audio);
    require(controller.audio_mixer().meters().synthVoices > 0U, "event audition did not reach synth mixer source");

    // Move a node through the same pointer path used by the native editor and verify undo/redo.
    auto movedRect = controller.audio_event_panel().layout().nodes[1];
    const auto oldPosition = controller.audio_event_panel().asset().editorNodes[1];
    controller.pointer_down(PointerButton::Primary, movedRect.x + 40, movedRect.y + 20);
    controller.pointer_move(movedRect.x + 100, movedRect.y + 70);
    controller.pointer_up(PointerButton::Primary, movedRect.x + 100, movedRect.y + 70);
    const auto movedPosition = controller.audio_event_panel().asset().editorNodes[1];
    require(std::fabs(movedPosition.x - oldPosition.x) > 1.0F,
            "node drag did not update serialized editor position");
    controller.key_down("z", true, false, false);
    require(std::fabs(controller.audio_event_panel().asset().editorNodes[1].x - oldPosition.x) < 0.1F,
            "graph undo did not restore node position");
    controller.key_down("y", true, false, false);
    require(std::fabs(controller.audio_event_panel().asset().editorNodes[1].x - movedPosition.x) < 0.1F,
            "graph redo did not restore moved node position");

    // Searchable palette input is routed through the editor text-input path.
    const auto search = controller.audio_event_panel().layout().paletteSearch;
    controller.pointer_down(PointerButton::Primary, search.x + 3, search.y + 3);
    controller.text_input("blend");
    require(controller.audio_event_panel().palette_filter() == "blend" &&
            controller.audio_event_panel().layout().paletteRowCount == 1,
            "palette search did not filter node types");
    controller.key_down("a", true, false, false);
    require(controller.audio_event_panel().palette_filter().empty(),
            "palette Ctrl+A did not clear the search");
    controller.key_down("escape", false, false, false);

    // Add and connect a subgraph using explicit authoring operations.
    auto& panel = controller.audio_event_panel();
    require(panel.add_node(audio::AudioEventNodeType::Layer, 120.0F, 350.0F), "failed to add layer node");
    const std::size_t layer = panel.asset().graph.nodes.size() - 1U;
    require(panel.add_node(audio::AudioEventNodeType::SynthNote, 340.0F, 350.0F), "failed to add note node");
    const std::size_t note = panel.asset().graph.nodes.size() - 1U;
    require(panel.connect_nodes(layer, note), "failed to connect authoring nodes");
    require(panel.report().succeeded(), "connected subgraph did not compile");
    require(!panel.connect_nodes(note, layer), "terminal node accepted an outgoing connection");
    require(!panel.connect_nodes(layer, layer), "cycle prevention accepted a self-edge");

    // Copy/paste the selected note and verify graph/remapping remains compilable.
    const auto noteRect = panel.layout().nodes[note];
    controller.pointer_down(PointerButton::Primary, noteRect.x + 30, noteRect.y + 20);
    controller.pointer_up(PointerButton::Primary, noteRect.x + 30, noteRect.y + 20);
    const auto properties = panel.selected_properties();
    require(std::any_of(properties.begin(), properties.end(), [](const auto& property) {
        return property.property == AudioEventNodeProperty::Note;
    }), "synth-note inspector did not expose MIDI note editing");
    const auto oldNote = panel.asset().graph.nodes[note].note;
    require(panel.adjust_selected_property(AudioEventNodeProperty::Note, 1) &&
                panel.asset().graph.nodes[note].note == oldNote + 1U,
            "node-specific property command did not adjust MIDI note");
    require(panel.undo() && panel.asset().graph.nodes[note].note == oldNote,
            "node-property undo did not restore MIDI note");
    require(panel.redo() && panel.asset().graph.nodes[note].note == oldNote + 1U,
            "node-property redo did not restore edited MIDI note");
    const auto copyCount = panel.asset().graph.nodes.size();
    require(panel.copy_selected() && panel.paste(), "copy/paste failed");
    require(panel.asset().graph.nodes.size() == copyCount + 1U && panel.report().succeeded(),
            "pasted graph node was not remapped or attached correctly");

    // Deletion is undoable even when it temporarily leaves a control node invalid.
    require(panel.delete_selected(), "delete selected failed");
    require(panel.undo(), "delete undo failed");
    require(panel.report().succeeded(), "undo did not restore a valid graph");

    // v4 persistence retains authoring coordinates and remains backward-independent of UI scale.
    const auto temp = std::filesystem::temp_directory_path() / "dve_editor_audio_event_v4.dveaudio";
    std::string error;
    require(panel.save(temp, &error), error.c_str());
    EditorAudioEventPanel loadedPanel;
    require(loadedPanel.open_asset(temp, &error), error.c_str());
    require(loadedPanel.asset().version == audio::kAudioEventAssetVersion &&
            loadedPanel.asset().editorNodes.size() == panel.asset().graph.nodes.size(),
            "v4 event asset did not preserve editor layout");
    require(std::fabs(loadedPanel.asset().editorNodes[1].x - panel.asset().editorNodes[1].x) < 0.1F,
            "v4 event asset changed node coordinates");
    std::filesystem::remove(temp);

    panel.asset().graph.nodes[0].children.push_back(999U);
    panel.compile();
    require(!panel.report().succeeded(), "invalid event edge escaped compiler diagnostics");
    std::cout << "DVE editor audio-event tests passed\n";
    return 0;
}
