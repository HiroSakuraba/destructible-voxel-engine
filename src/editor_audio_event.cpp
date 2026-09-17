#include "dve/editor_audio_event.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace dve::editor {
namespace {

constexpr std::array<audio::AudioEventNodeType, kAudioEventPaletteEntries> kPaletteTypes{
    audio::AudioEventNodeType::Sample,
    audio::AudioEventNodeType::Stream,
    audio::AudioEventNodeType::SynthNote,
    audio::AudioEventNodeType::Layer,
    audio::AudioEventNodeType::RandomNoRepeat,
    audio::AudioEventNodeType::Sequence,
    audio::AudioEventNodeType::Delay,
    audio::AudioEventNodeType::Gain,
    audio::AudioEventNodeType::Bus,
    audio::AudioEventNodeType::Switch,
    audio::AudioEventNodeType::Scatter,
    audio::AudioEventNodeType::Cooldown,
    audio::AudioEventNodeType::Blend,
    audio::AudioEventNodeType::Loop,
};

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

bool matches_filter(audio::AudioEventNodeType type, std::string_view filter) {
    if (filter.empty()) return true;
    const std::string name(audio::audio_event_node_type_name(type));
    return lowercase(name).find(lowercase(filter)) != std::string::npos;
}

void erase_last_utf8_codepoint(std::string& text) {
    if (text.empty()) return;
    std::size_t erase = text.size() - 1U;
    while (erase > 0U && (static_cast<unsigned char>(text[erase]) & 0xC0U) == 0x80U) --erase;
    text.erase(erase);
}

} // namespace

EditorAudioEventPanel::EditorAudioEventPanel() {
    asset_.graph.name = "editor/preview_event";
    asset_.graph.root = 0;
    asset_.graph.nodes.resize(5);
    asset_.graph.nodes[0].type = audio::AudioEventNodeType::Layer;
    asset_.graph.nodes[0].children = {1, 2};
    asset_.graph.nodes[1].type = audio::AudioEventNodeType::SynthNote;
    asset_.graph.nodes[1].note = 48;
    asset_.graph.nodes[1].velocity = 0.65F;
    asset_.graph.nodes[1].durationSeconds = 0.25F;
    asset_.graph.nodes[2].type = audio::AudioEventNodeType::RandomNoRepeat;
    asset_.graph.nodes[2].children = {3, 4};
    asset_.graph.nodes[3].type = audio::AudioEventNodeType::SynthNote;
    asset_.graph.nodes[3].note = 55;
    asset_.graph.nodes[3].durationSeconds = 0.18F;
    asset_.graph.nodes[4].type = audio::AudioEventNodeType::SynthNote;
    asset_.graph.nodes[4].note = 60;
    asset_.graph.nodes[4].durationSeconds = 0.18F;
    asset_.editorNodes = {{20.0F, 150.0F}, {220.0F, 80.0F}, {220.0F, 220.0F},
                          {420.0F, 180.0F}, {420.0F, 260.0F}};
    selectedNode_ = 1U; // Start on a parameterized node so the inspector is immediately useful.
    compile();
    history_.push_back(asset_);
    set_status("Ready");
}

void EditorAudioEventPanel::resize(int width, int height, float uiScale) noexcept {
    const int panelWidth = std::clamp(static_cast<int>(1160.0F * uiScale), 900, std::max(900, width - 30));
    const int panelHeight = std::clamp(static_cast<int>(680.0F * uiScale), 520, std::max(520, height - 50));
    const int x = std::max(15, (width - panelWidth) / 2);
    const int y = std::max(35, (height - panelHeight) / 2);
    layout_.panel = {x, y, panelWidth, panelHeight};
    layout_.titleBar = {x, y, panelWidth, 34};
    layout_.closeButton = {x + panelWidth - 31, y + 5, 24, 24};

    int bx = x + 12;
    const int by = y + 44;
    auto place = [&](UiRect& rect, int widthPixels) {
        rect = {bx, by, widthPixels, 26};
        bx += widthPixels + 6;
    };
    place(layout_.compileButton, 76);
    place(layout_.auditionButton, 78);
    place(layout_.addNoteButton, 70);
    place(layout_.saveButton, 54);
    place(layout_.openButton, 54);
    place(layout_.undoButton, 54);
    place(layout_.redoButton, 54);
    place(layout_.copyButton, 54);
    place(layout_.pasteButton, 58);
    place(layout_.deleteButton, 62);
    place(layout_.reseedButton, 66);

    const int paletteWidth = 172;
    const int inspectorWidth = 242;
    const int contentY = y + 80;
    const int contentHeight = panelHeight - 104;
    layout_.palette = {x + 12, contentY, paletteWidth, contentHeight};
    layout_.paletteSearch = {layout_.palette.x + 8, layout_.palette.y + 28, paletteWidth - 16, 25};
    layout_.canvas = {layout_.palette.x + paletteWidth + 8, contentY,
                      panelWidth - paletteWidth - inspectorWidth - 46, contentHeight};
    layout_.inspector = {x + panelWidth - inspectorWidth - 10, contentY, inspectorWidth, contentHeight};
    layout_.propertyRowCount = asset_.graph.nodes.empty() ? 0U : kAudioEventPropertyRows;
    for (std::size_t row = 0; row < kAudioEventPropertyRows; ++row) {
        const int rowY = layout_.inspector.y + 119 + static_cast<int>(row) * 29;
        layout_.propertyMinusButtons[row] = {layout_.inspector.x + layout_.inspector.width - 55,
                                             rowY, 22, 22};
        layout_.propertyPlusButtons[row] = {layout_.inspector.x + layout_.inspector.width - 29,
                                            rowY, 22, 22};
    }
    ensure_editor_positions();
    rebuild_palette_layout();
    rebuild_node_layout();
}

void EditorAudioEventPanel::ensure_editor_positions() {
    if (asset_.editorNodes.size() == asset_.graph.nodes.size()) return;
    asset_.editorNodes.resize(asset_.graph.nodes.size());
    std::array<std::uint32_t, 16> rows{};
    std::vector<std::uint32_t> depth(asset_.graph.nodes.size());
    std::vector<bool> visited(asset_.graph.nodes.size());
    auto assign = [&](auto&& self, std::uint32_t index, std::uint32_t d) -> void {
        if (index >= asset_.graph.nodes.size()) return;
        depth[index] = std::max(depth[index], d);
        if (visited[index]) return;
        visited[index] = true;
        for (const auto child : asset_.graph.nodes[index].children) self(self, child, d + 1U);
    };
    if (!asset_.graph.nodes.empty()) assign(assign, asset_.graph.root, 0U);
    for (std::size_t i = 0; i < asset_.graph.nodes.size(); ++i) {
        if (asset_.editorNodes[i].x != 0.0F || asset_.editorNodes[i].y != 0.0F) continue;
        const std::uint32_t d = std::min<std::uint32_t>(depth[i], 15U);
        const std::uint32_t row = rows[d]++;
        asset_.editorNodes[i] = {20.0F + static_cast<float>(d) * 185.0F,
                                 20.0F + static_cast<float>(row) * 72.0F};
    }
}

void EditorAudioEventPanel::rebuild_palette_layout() noexcept {
    layout_.paletteRowCount = 0;
    int y = layout_.paletteSearch.y + layout_.paletteSearch.height + 8;
    for (const auto type : kPaletteTypes) {
        if (!matches_filter(type, paletteFilter_)) continue;
        if (layout_.paletteRowCount >= layout_.paletteRows.size()) break;
        layout_.paletteRows[layout_.paletteRowCount++] = {
            layout_.palette.x + 8, y, layout_.palette.width - 16, 25};
        y += 29;
    }
}

void EditorAudioEventPanel::rebuild_node_layout() noexcept {
    ensure_editor_positions();
    layout_.nodeCount = std::min<std::size_t>(asset_.graph.nodes.size(), layout_.nodes.size());
    constexpr int nodeWidth = 156;
    constexpr int nodeHeight = 54;
    constexpr int portSize = 12;
    const int maximumX = std::max(0, layout_.canvas.width - nodeWidth - 8);
    const int maximumY = std::max(0, layout_.canvas.height - nodeHeight - 8);
    for (std::size_t i = 0; i < layout_.nodeCount; ++i) {
        const auto& position = asset_.editorNodes[i];
        const int localX = std::clamp(static_cast<int>(std::lround(position.x)), 4, maximumX);
        const int localY = std::clamp(static_cast<int>(std::lround(position.y)), 4, maximumY);
        layout_.nodes[i] = {layout_.canvas.x + localX, layout_.canvas.y + localY, nodeWidth, nodeHeight};
        layout_.inputPorts[i] = {layout_.nodes[i].x - portSize / 2,
                                 layout_.nodes[i].y + nodeHeight / 2 - portSize / 2,
                                 portSize, portSize};
        layout_.outputPorts[i] = {layout_.nodes[i].x + nodeWidth - portSize / 2,
                                  layout_.nodes[i].y + nodeHeight / 2 - portSize / 2,
                                  portSize, portSize};
    }
}

void EditorAudioEventPanel::compile() {
    ensure_editor_positions();
    report_ = audio::compile_audio_event_report(asset_);
}

void EditorAudioEventPanel::audition(audio::AudioMixer& mixer) noexcept {
    if (!report_.event) {
        set_status("Cannot audition until compile errors are fixed");
        return;
    }
    static audio::AudioEventInstanceState state;
    audio::AudioEventParameters parameters;
    parameters.values["intensity"] = 0.5F;
    const auto execution = audio::execute_audio_event(*report_.event, parameters, state,
                                                       mixer.sample_rate(), mixer.current_frame());
    audio::dispatch_audio_event(execution, mixer);
    set_status("Auditioned " + std::to_string(execution.count) + " scheduled actions");
}

void EditorAudioEventPanel::commit_history() {
    if (historyCursor_ + 1U < history_.size())
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(historyCursor_ + 1U), history_.end());
    history_.push_back(asset_);
    if (history_.size() > kAudioEventUndoDepth) history_.erase(history_.begin());
    historyCursor_ = history_.empty() ? 0U : history_.size() - 1U;
}

void EditorAudioEventPanel::restore_history(std::size_t index) {
    if (index >= history_.size()) return;
    historyCursor_ = index;
    asset_ = history_[historyCursor_];
    selectedNode_ = asset_.graph.nodes.empty() ? 0U : std::min(selectedNode_, asset_.graph.nodes.size() - 1U);
    compile();
    rebuild_node_layout();
}

bool EditorAudioEventPanel::undo() {
    if (!can_undo()) return false;
    restore_history(historyCursor_ - 1U);
    set_status("Undid graph edit");
    return true;
}

bool EditorAudioEventPanel::redo() {
    if (!can_redo()) return false;
    restore_history(historyCursor_ + 1U);
    set_status("Redid graph edit");
    return true;
}

std::size_t EditorAudioEventPanel::maximum_children(audio::AudioEventNodeType type) noexcept {
    switch (type) {
        case audio::AudioEventNodeType::Sample:
        case audio::AudioEventNodeType::Stream:
        case audio::AudioEventNodeType::SynthNote: return 0U;
        case audio::AudioEventNodeType::Delay:
        case audio::AudioEventNodeType::Gain:
        case audio::AudioEventNodeType::Bus:
        case audio::AudioEventNodeType::Scatter:
        case audio::AudioEventNodeType::Cooldown:
        case audio::AudioEventNodeType::Loop: return 1U;
        case audio::AudioEventNodeType::Switch:
        case audio::AudioEventNodeType::Blend: return 2U;
        case audio::AudioEventNodeType::RandomNoRepeat:
        case audio::AudioEventNodeType::Sequence:
        case audio::AudioEventNodeType::Layer: return 256U;
    }
    return 0U;
}

bool EditorAudioEventPanel::add_node(audio::AudioEventNodeType type, float x, float y) {
    if (asset_.graph.nodes.size() >= kMaxVisibleAudioEventNodes) {
        set_status("Visible graph limit reached");
        return false;
    }
    audio::AudioEventNode node;
    node.type = type;
    if (type == audio::AudioEventNodeType::SynthNote) {
        node.note = static_cast<std::uint8_t>(48U + (asset_.graph.nodes.size() * 5U) % 36U);
        node.velocity = 0.65F;
        node.durationSeconds = 0.20F;
    } else if (type == audio::AudioEventNodeType::Delay) {
        node.value = 0.05F;
    } else if (type == audio::AudioEventNodeType::Cooldown) {
        node.value = 0.1F;
    } else if (type == audio::AudioEventNodeType::Scatter) {
        node.count = 3;
        node.value2 = 0.04F;
    } else if (type == audio::AudioEventNodeType::Blend) {
        node.parameter = "intensity";
        node.threshold = 0.0F;
        node.value2 = 1.0F;
    } else if (type == audio::AudioEventNodeType::Switch) {
        node.parameter = "state";
        node.threshold = 0.5F;
    }
    const auto index = asset_.graph.nodes.size();
    asset_.graph.nodes.push_back(std::move(node));
    asset_.editorNodes.push_back({x, y});
    selectedNode_ = index;
    compile();
    rebuild_node_layout();
    commit_history();
    set_status("Added " + std::string(audio::audio_event_node_type_name(type)) + " node");
    return true;
}

bool EditorAudioEventPanel::would_create_cycle(std::size_t parent, std::size_t child) const noexcept {
    if (parent == child || parent >= asset_.graph.nodes.size() || child >= asset_.graph.nodes.size()) return true;
    std::vector<std::size_t> stack{child};
    std::vector<bool> visited(asset_.graph.nodes.size());
    while (!stack.empty()) {
        const auto current = stack.back();
        stack.pop_back();
        if (current == parent) return true;
        if (current >= visited.size() || visited[current]) continue;
        visited[current] = true;
        for (const auto next : asset_.graph.nodes[current].children) stack.push_back(next);
    }
    return false;
}

bool EditorAudioEventPanel::connect_nodes(std::size_t parent, std::size_t child) {
    if (parent >= asset_.graph.nodes.size() || child >= asset_.graph.nodes.size()) return false;
    auto& children = asset_.graph.nodes[parent].children;
    if (maximum_children(asset_.graph.nodes[parent].type) == 0U) {
        set_status("Terminal nodes cannot have outgoing links");
        return false;
    }
    if (std::find(children.begin(), children.end(), static_cast<std::uint32_t>(child)) != children.end()) {
        set_status("Link already exists");
        return false;
    }
    if (children.size() >= maximum_children(asset_.graph.nodes[parent].type)) {
        set_status("Node has reached its child-link limit");
        return false;
    }
    if (would_create_cycle(parent, child)) {
        set_status("Link rejected because it would create a cycle");
        return false;
    }
    children.push_back(static_cast<std::uint32_t>(child));
    compile();
    rebuild_node_layout();
    commit_history();
    set_status("Connected node " + std::to_string(parent) + " to " + std::to_string(child));
    return true;
}

std::vector<std::size_t> EditorAudioEventPanel::selected_subgraph() const {
    std::vector<std::size_t> result;
    if (selectedNode_ >= asset_.graph.nodes.size()) return result;
    std::vector<std::size_t> stack{selectedNode_};
    std::vector<bool> visited(asset_.graph.nodes.size());
    while (!stack.empty()) {
        const auto current = stack.back();
        stack.pop_back();
        if (current >= visited.size() || visited[current]) continue;
        visited[current] = true;
        result.push_back(current);
        for (const auto child : asset_.graph.nodes[current].children) stack.push_back(child);
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool EditorAudioEventPanel::copy_selected() {
    const auto indices = selected_subgraph();
    if (indices.empty()) return false;
    std::unordered_map<std::size_t, std::size_t> remap;
    for (std::size_t i = 0; i < indices.size(); ++i) remap.emplace(indices[i], i);
    clipboard_ = {};
    clipboard_.nodes.reserve(indices.size());
    clipboard_.positions.reserve(indices.size());
    for (const auto oldIndex : indices) {
        auto node = asset_.graph.nodes[oldIndex];
        std::vector<std::uint32_t> remapped;
        for (const auto child : node.children) {
            const auto found = remap.find(child);
            if (found != remap.end()) remapped.push_back(static_cast<std::uint32_t>(found->second));
        }
        node.children = std::move(remapped);
        clipboard_.nodes.push_back(std::move(node));
        clipboard_.positions.push_back(asset_.editorNodes[oldIndex]);
    }
    clipboard_.root = remap.at(selectedNode_);
    clipboard_.valid = true;
    set_status("Copied " + std::to_string(indices.size()) + " graph nodes");
    return true;
}

bool EditorAudioEventPanel::paste() {
    if (!clipboard_.valid || clipboard_.nodes.empty()) return false;
    if (asset_.graph.nodes.size() + clipboard_.nodes.size() > kMaxVisibleAudioEventNodes) {
        set_status("Paste exceeds visible graph limit");
        return false;
    }
    const std::size_t base = asset_.graph.nodes.size();
    for (std::size_t i = 0; i < clipboard_.nodes.size(); ++i) {
        auto node = clipboard_.nodes[i];
        for (auto& child : node.children) child = static_cast<std::uint32_t>(base + child);
        asset_.graph.nodes.push_back(std::move(node));
        auto position = clipboard_.positions[i];
        position.x += 28.0F;
        position.y += 28.0F;
        asset_.editorNodes.push_back(position);
    }
    const std::size_t pastedRoot = base + clipboard_.root;
    if (asset_.graph.root < base && asset_.graph.nodes[asset_.graph.root].type == audio::AudioEventNodeType::Layer)
        asset_.graph.nodes[asset_.graph.root].children.push_back(static_cast<std::uint32_t>(pastedRoot));
    selectedNode_ = pastedRoot;
    compile();
    rebuild_node_layout();
    commit_history();
    set_status("Pasted " + std::to_string(clipboard_.nodes.size()) + " graph nodes");
    return true;
}

bool EditorAudioEventPanel::delete_selected() {
    if (asset_.graph.nodes.empty() || selectedNode_ >= asset_.graph.nodes.size()) return false;
    if (selectedNode_ == asset_.graph.root) {
        set_status("The root node cannot be deleted");
        return false;
    }
    const auto removed = selectedNode_;
    asset_.graph.nodes.erase(asset_.graph.nodes.begin() + static_cast<std::ptrdiff_t>(removed));
    asset_.editorNodes.erase(asset_.editorNodes.begin() + static_cast<std::ptrdiff_t>(removed));
    for (auto& node : asset_.graph.nodes) {
        node.children.erase(std::remove(node.children.begin(), node.children.end(), static_cast<std::uint32_t>(removed)),
                            node.children.end());
        for (auto& child : node.children) if (child > removed) --child;
    }
    if (asset_.graph.root > removed) --asset_.graph.root;
    selectedNode_ = std::min<std::size_t>(removed, asset_.graph.nodes.size() - 1U);
    compile();
    rebuild_node_layout();
    commit_history();
    set_status("Deleted graph node");
    return true;
}

bool EditorAudioEventPanel::save(const std::filesystem::path& path, std::string* error) {
    compile();
    if (!audio::write_audio_event_asset(path, asset_, error)) {
        set_status(error && !error->empty() ? *error : "Save failed");
        return false;
    }
    defaultPath_ = path;
    set_status("Saved " + path.string());
    return true;
}

bool EditorAudioEventPanel::open_asset(const std::filesystem::path& path, std::string* error) {
    auto loaded = audio::read_audio_event_asset(path, error);
    if (!loaded) {
        set_status(error && !error->empty() ? *error : "Open failed");
        return false;
    }
    asset_ = std::move(*loaded);
    ensure_editor_positions();
    selectedNode_ = std::min<std::size_t>(asset_.graph.root, asset_.graph.nodes.size() - 1U);
    compile();
    rebuild_node_layout();
    history_.clear();
    history_.push_back(asset_);
    historyCursor_ = 0;
    defaultPath_ = path;
    set_status("Opened " + path.string());
    return true;
}

std::vector<AudioEventPropertyView> EditorAudioEventPanel::selected_properties() const {
    std::vector<AudioEventPropertyView> properties;
    if (asset_.graph.nodes.empty() || selectedNode_ >= asset_.graph.nodes.size()) return properties;
    const auto& node = asset_.graph.nodes[selectedNode_];
    auto number = [](float value, int precision = 2) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(precision) << value;
        return stream.str();
    };
    auto add = [&](AudioEventNodeProperty property, std::string label,
                   std::string value, std::string unit = {}) {
        if (properties.size() < kAudioEventPropertyRows)
            properties.push_back({property, std::move(label), std::move(value), std::move(unit)});
    };
    switch (node.type) {
        case audio::AudioEventNodeType::Sample:
            add(AudioEventNodeProperty::SampleId, "Sample", std::to_string(node.sample.value));
            add(AudioEventNodeProperty::Bus, "Bus", std::to_string(static_cast<unsigned>(node.bus)));
            add(AudioEventNodeProperty::Priority, "Priority", std::to_string(static_cast<unsigned>(node.priority)));
            break;
        case audio::AudioEventNodeType::Stream:
            add(AudioEventNodeProperty::StreamId, "Stream", std::to_string(node.stream.value));
            add(AudioEventNodeProperty::Loop, "Loop", node.loop ? "On" : "Off");
            add(AudioEventNodeProperty::Bus, "Bus", std::to_string(static_cast<unsigned>(node.bus)));
            break;
        case audio::AudioEventNodeType::SynthNote:
            add(AudioEventNodeProperty::Note, "MIDI note", std::to_string(node.note));
            add(AudioEventNodeProperty::Velocity, "Velocity", number(node.velocity));
            add(AudioEventNodeProperty::Duration, "Duration", number(node.durationSeconds), "s");
            add(AudioEventNodeProperty::Priority, "Priority", std::to_string(static_cast<unsigned>(node.priority)));
            break;
        case audio::AudioEventNodeType::RandomNoRepeat:
            add(AudioEventNodeProperty::NoRepeatHistory, "No-repeat", std::to_string(asset_.noRepeatHistory));
            break;
        case audio::AudioEventNodeType::Delay:
            add(AudioEventNodeProperty::Value, "Delay", number(node.value), "s");
            break;
        case audio::AudioEventNodeType::Gain:
            add(AudioEventNodeProperty::Value, "Gain", number(node.value));
            break;
        case audio::AudioEventNodeType::Bus:
            add(AudioEventNodeProperty::Bus, "Bus", std::to_string(static_cast<unsigned>(node.bus)));
            break;
        case audio::AudioEventNodeType::Switch:
            add(AudioEventNodeProperty::Threshold, "Threshold", number(node.threshold));
            break;
        case audio::AudioEventNodeType::Scatter:
            add(AudioEventNodeProperty::Value, "Delay min", number(node.value), "s");
            add(AudioEventNodeProperty::Value2, "Delay max", number(node.value2), "s");
            add(AudioEventNodeProperty::Count, "Count", std::to_string(node.count));
            break;
        case audio::AudioEventNodeType::Cooldown:
            add(AudioEventNodeProperty::Value, "Cooldown", number(node.value), "s");
            break;
        case audio::AudioEventNodeType::Blend:
            add(AudioEventNodeProperty::Threshold, "Range min", number(node.threshold));
            add(AudioEventNodeProperty::Value2, "Range max", number(node.value2));
            add(AudioEventNodeProperty::Curve, "Curve", std::to_string(static_cast<unsigned>(node.curve)));
            break;
        case audio::AudioEventNodeType::Loop:
            add(AudioEventNodeProperty::Value, "Interval", number(node.value), "s");
            add(AudioEventNodeProperty::Count, "Count", std::to_string(node.count));
            break;
        case audio::AudioEventNodeType::Layer:
        case audio::AudioEventNodeType::Sequence:
            break;
    }
    return properties;
}

bool EditorAudioEventPanel::adjust_selected_property(AudioEventNodeProperty property, int steps) {
    if (steps == 0 || asset_.graph.nodes.empty() || selectedNode_ >= asset_.graph.nodes.size())
        return false;
    const auto editable = selected_properties();
    const auto descriptor = std::find_if(editable.begin(), editable.end(),
                                         [property](const auto& item) {
                                             return item.property == property;
                                         });
    if (descriptor == editable.end()) return false;
    const std::string propertyLabel = descriptor->label;
    auto& node = asset_.graph.nodes[selectedNode_];
    const auto clamp_float = [steps](float value, float increment, float minimum, float maximum) {
        return std::clamp(value + increment * static_cast<float>(steps), minimum, maximum);
    };
    switch (property) {
        case AudioEventNodeProperty::SampleId: {
            const std::int64_t value = static_cast<std::int64_t>(node.sample.value) + steps;
            node.sample.value = static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, UINT32_MAX));
            break;
        }
        case AudioEventNodeProperty::StreamId: {
            const std::int64_t value = static_cast<std::int64_t>(node.stream.value) + steps;
            node.stream.value = static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, UINT32_MAX));
            break;
        }
        case AudioEventNodeProperty::Loop: node.loop = !node.loop; break;
        case AudioEventNodeProperty::Bus: {
            int value = static_cast<int>(node.bus) + steps;
            const int count = static_cast<int>(audio::kAudioBusCount);
            value = ((value % count) + count) % count;
            node.bus = static_cast<audio::AudioBusId>(value);
            break;
        }
        case AudioEventNodeProperty::Priority: {
            constexpr std::array<audio::AudioPriority, 6> priorities{
                audio::AudioPriority::Background, audio::AudioPriority::Low,
                audio::AudioPriority::Normal, audio::AudioPriority::Important,
                audio::AudioPriority::Hero, audio::AudioPriority::Critical};
            auto found = std::find(priorities.begin(), priorities.end(), node.priority);
            int index = found == priorities.end() ? 2 : static_cast<int>(found - priorities.begin());
            index = std::clamp(index + steps, 0, static_cast<int>(priorities.size() - 1U));
            node.priority = priorities[static_cast<std::size_t>(index)];
            break;
        }
        case AudioEventNodeProperty::Note: {
            const int value = static_cast<int>(node.note) + steps;
            node.note = static_cast<std::uint8_t>(std::clamp(value, 0, 127));
            break;
        }
        case AudioEventNodeProperty::Velocity:
            node.velocity = clamp_float(node.velocity, 0.05F, 0.0F, 1.0F); break;
        case AudioEventNodeProperty::Duration:
            node.durationSeconds = clamp_float(node.durationSeconds, 0.05F, 0.01F, 30.0F); break;
        case AudioEventNodeProperty::Value: {
            float increment = 0.05F;
            float maximum = 30.0F;
            if (node.type == audio::AudioEventNodeType::Gain) { increment = 0.05F; maximum = 4.0F; }
            node.value = clamp_float(node.value, increment, 0.0F, maximum);
            break;
        }
        case AudioEventNodeProperty::Value2:
            node.value2 = clamp_float(node.value2, 0.05F, -1000.0F, 1000.0F); break;
        case AudioEventNodeProperty::Count: {
            const std::int64_t value = static_cast<std::int64_t>(node.count) + steps;
            node.count = static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 1, 64));
            break;
        }
        case AudioEventNodeProperty::Threshold:
            node.threshold = clamp_float(node.threshold, 0.1F, -1000.0F, 1000.0F); break;
        case AudioEventNodeProperty::Curve: {
            int value = static_cast<int>(node.curve) + steps;
            value = ((value % 3) + 3) % 3;
            node.curve = static_cast<audio::AudioBlendCurve>(value);
            break;
        }
        case AudioEventNodeProperty::NoRepeatHistory: {
            const std::int64_t value = static_cast<std::int64_t>(asset_.noRepeatHistory) + steps;
            asset_.noRepeatHistory = static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 1, 16));
            break;
        }
    }
    compile();
    commit_history();
    set_status("Changed " + propertyLabel);
    return true;
}

std::optional<std::size_t> EditorAudioEventPanel::node_at(int x, int y) const noexcept {
    for (std::size_t i = layout_.nodeCount; i > 0; --i)
        if (layout_.nodes[i - 1U].contains(x, y)) return i - 1U;
    return std::nullopt;
}

std::optional<std::size_t> EditorAudioEventPanel::input_port_at(int x, int y) const noexcept {
    for (std::size_t i = 0; i < layout_.nodeCount; ++i)
        if (layout_.inputPorts[i].contains(x, y)) return i;
    return std::nullopt;
}

std::optional<std::size_t> EditorAudioEventPanel::output_port_at(int x, int y) const noexcept {
    for (std::size_t i = 0; i < layout_.nodeCount; ++i)
        if (layout_.outputPorts[i].contains(x, y)) return i;
    return std::nullopt;
}

std::optional<audio::AudioEventNodeType> EditorAudioEventPanel::palette_type_at(int x, int y) const noexcept {
    std::size_t visible{};
    for (const auto type : kPaletteTypes) {
        if (!matches_filter(type, paletteFilter_)) continue;
        if (visible < layout_.paletteRowCount && layout_.paletteRows[visible].contains(x, y)) return type;
        ++visible;
    }
    return std::nullopt;
}

bool EditorAudioEventPanel::pointer_down(int x, int y, audio::AudioMixer& mixer) noexcept {
    if (!open_ || !layout_.panel.contains(x, y)) return false;
    pointerX_ = x;
    pointerY_ = y;
    if (layout_.closeButton.contains(x, y)) { open_ = false; return true; }
    if (layout_.compileButton.contains(x, y)) {
        compile();
        set_status(report_.succeeded() ? "Compile succeeded" : "Compile reported errors");
        return true;
    }
    if (layout_.auditionButton.contains(x, y)) { audition(mixer); return true; }
    if (layout_.addNoteButton.contains(x, y)) {
        const float px = 30.0F + static_cast<float>((asset_.graph.nodes.size() % 3U) * 28U);
        const float py = 30.0F + static_cast<float>((asset_.graph.nodes.size() % 6U) * 65U);
        const std::size_t before = asset_.graph.nodes.size();
        if (add_node(audio::AudioEventNodeType::SynthNote, px, py) &&
            asset_.graph.root < before && asset_.graph.nodes[asset_.graph.root].type == audio::AudioEventNodeType::Layer) {
            asset_.graph.nodes[asset_.graph.root].children.push_back(static_cast<std::uint32_t>(before));
            compile();
            history_.back() = asset_;
        }
        return true;
    }
    if (layout_.saveButton.contains(x, y)) { std::string error; (void)save(defaultPath_, &error); return true; }
    if (layout_.openButton.contains(x, y)) { std::string error; (void)open_asset(defaultPath_, &error); return true; }
    if (layout_.undoButton.contains(x, y)) { (void)undo(); return true; }
    if (layout_.redoButton.contains(x, y)) { (void)redo(); return true; }
    if (layout_.copyButton.contains(x, y)) { (void)copy_selected(); return true; }
    if (layout_.pasteButton.contains(x, y)) { (void)paste(); return true; }
    if (layout_.deleteButton.contains(x, y)) { (void)delete_selected(); return true; }
    if (layout_.reseedButton.contains(x, y)) {
        asset_.deterministicSeed = asset_.deterministicSeed * 1664525U + 1013904223U;
        compile();
        commit_history();
        set_status("Changed deterministic audition seed");
        return true;
    }
    const auto properties = selected_properties();
    for (std::size_t row = 0; row < properties.size() && row < layout_.propertyRowCount; ++row) {
        if (layout_.propertyMinusButtons[row].contains(x, y)) {
            (void)adjust_selected_property(properties[row].property, -1);
            return true;
        }
        if (layout_.propertyPlusButtons[row].contains(x, y)) {
            (void)adjust_selected_property(properties[row].property, 1);
            return true;
        }
    }
    if (layout_.paletteSearch.contains(x, y)) {
        searchActive_ = true;
        return true;
    }
    searchActive_ = false;
    if (const auto type = palette_type_at(x, y)) {
        const float px = 30.0F + static_cast<float>((asset_.graph.nodes.size() % 3U) * 28U);
        const float py = 30.0F + static_cast<float>((asset_.graph.nodes.size() % 6U) * 65U);
        (void)add_node(*type, px, py);
        return true;
    }
    if (const auto output = output_port_at(x, y)) {
        selectedNode_ = *output;
        interaction_ = Interaction::Connect;
        interactionNode_ = *output;
        return true;
    }
    if (const auto node = node_at(x, y)) {
        selectedNode_ = *node;
        interaction_ = Interaction::MoveNode;
        interactionNode_ = *node;
        dragOffsetX_ = x - layout_.nodes[*node].x;
        dragOffsetY_ = y - layout_.nodes[*node].y;
        dragStartPosition_ = asset_.editorNodes[*node];
        dragChanged_ = false;
        return true;
    }
    if (layout_.canvas.contains(x, y)) interaction_ = Interaction::Inactive;
    return true;
}

bool EditorAudioEventPanel::pointer_move(int x, int y) noexcept {
    if (!open_) return false;
    pointerX_ = x;
    pointerY_ = y;
    if (interaction_ == Interaction::MoveNode && interactionNode_ < asset_.editorNodes.size()) {
        const float localX = static_cast<float>(x - layout_.canvas.x - dragOffsetX_);
        const float localY = static_cast<float>(y - layout_.canvas.y - dragOffsetY_);
        const float maximumX = static_cast<float>(std::max(4, layout_.canvas.width - 164));
        const float maximumY = static_cast<float>(std::max(4, layout_.canvas.height - 62));
        auto& position = asset_.editorNodes[interactionNode_];
        position.x = std::clamp(localX, 4.0F, maximumX);
        position.y = std::clamp(localY, 4.0F, maximumY);
        dragChanged_ = std::fabs(position.x - dragStartPosition_.x) > 0.5F ||
                       std::fabs(position.y - dragStartPosition_.y) > 0.5F;
        rebuild_node_layout();
        return true;
    }
    return interaction_ == Interaction::Connect || layout_.panel.contains(x, y);
}

bool EditorAudioEventPanel::pointer_up(int x, int y) noexcept {
    if (!open_) return false;
    pointerX_ = x;
    pointerY_ = y;
    if (interaction_ == Interaction::MoveNode) {
        if (dragChanged_) {
            commit_history();
            set_status("Moved graph node");
        }
        interaction_ = Interaction::Inactive;
        return true;
    }
    if (interaction_ == Interaction::Connect) {
        const auto source = interactionNode_;
        interaction_ = Interaction::Inactive;
        if (const auto target = input_port_at(x, y)) (void)connect_nodes(source, *target);
        else set_status("Connection cancelled");
        return true;
    }
    return layout_.panel.contains(x, y);
}

bool EditorAudioEventPanel::key_down(std::string_view key, bool control, bool shift, bool alt) noexcept {
    if (!open_) return false;
    const std::string normalized = lowercase(key);
    if (searchActive_) {
        if (normalized == "escape" || normalized == "return" || normalized == "enter") {
            searchActive_ = false;
            return true;
        }
        if (normalized == "backspace") {
            erase_last_utf8_codepoint(paletteFilter_);
            rebuild_palette_layout();
            return true;
        }
        if (control && normalized == "a") {
            paletteFilter_.clear();
            rebuild_palette_layout();
            return true;
        }
        return !control && !alt;
    }
    if (control && normalized == "z") return shift ? redo() : undo();
    if (control && normalized == "y") return redo();
    if (control && normalized == "c") return copy_selected();
    if (control && normalized == "v") return paste();
    if (control && normalized == "s") { std::string error; return save(defaultPath_, &error); }
    if (control && normalized == "o") { std::string error; return open_asset(defaultPath_, &error); }
    if (control && normalized == "f") { searchActive_ = true; return true; }
    if (normalized == "delete" || normalized == "backspace") return delete_selected();
    if (normalized == "escape" && interaction_ != Interaction::Inactive) {
        if (interaction_ == Interaction::MoveNode && interactionNode_ < asset_.editorNodes.size()) {
            asset_.editorNodes[interactionNode_] = dragStartPosition_;
            rebuild_node_layout();
        }
        interaction_ = Interaction::Inactive;
        set_status("Edit cancelled");
        return true;
    }
    (void)alt;
    return false;
}

bool EditorAudioEventPanel::text_input(std::string_view text) noexcept {
    if (!open_ || !searchActive_ || text.empty()) return false;
    paletteFilter_.append(text);
    rebuild_palette_layout();
    return true;
}

} // namespace dve::editor
