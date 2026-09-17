#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/audio/event_graph.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

inline constexpr std::size_t kMaxVisibleAudioEventNodes = 64;
inline constexpr std::size_t kAudioEventPaletteEntries = 14;
inline constexpr std::size_t kAudioEventUndoDepth = 64;
inline constexpr std::size_t kAudioEventPropertyRows = 8;

enum class AudioEventNodeProperty : std::uint8_t {
    SampleId, StreamId, Loop, Bus, Priority, Note, Velocity, Duration,
    Value, Value2, Count, Threshold, Curve, NoRepeatHistory
};

struct AudioEventPropertyView {
    AudioEventNodeProperty property{};
    std::string label;
    std::string value;
    std::string unit;
};

struct AudioEventPanelLayout {
    UiRect panel{};
    UiRect titleBar{};
    UiRect closeButton{};
    UiRect compileButton{};
    UiRect auditionButton{};
    UiRect addNoteButton{};
    UiRect saveButton{};
    UiRect openButton{};
    UiRect undoButton{};
    UiRect redoButton{};
    UiRect copyButton{};
    UiRect pasteButton{};
    UiRect deleteButton{};
    UiRect reseedButton{};
    UiRect palette{};
    UiRect paletteSearch{};
    std::array<UiRect, kAudioEventPaletteEntries> paletteRows{};
    std::size_t paletteRowCount{};
    UiRect canvas{};
    UiRect inspector{};
    std::array<UiRect, kAudioEventPropertyRows> propertyMinusButtons{};
    std::array<UiRect, kAudioEventPropertyRows> propertyPlusButtons{};
    std::size_t propertyRowCount{};
    std::array<UiRect, kMaxVisibleAudioEventNodes> nodes{};
    std::array<UiRect, kMaxVisibleAudioEventNodes> inputPorts{};
    std::array<UiRect, kMaxVisibleAudioEventNodes> outputPorts{};
    std::size_t nodeCount{};
};

// An editor-facing graph authoring surface. The panel owns only serializable authoring state;
// compiled events are disposable products. Mutations are snapshot-undoable and file persistence
// goes through the canonical .dveaudio serializer rather than a second editor-only format.
class EditorAudioEventPanel {
public:
    EditorAudioEventPanel();
    [[nodiscard]] bool open() const noexcept { return open_; }
    void set_open(bool value) noexcept { open_ = value; }
    void toggle() noexcept { open_ = !open_; }
    [[nodiscard]] const AudioEventPanelLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] const audio::AudioEventAsset& asset() const noexcept { return asset_; }
    [[nodiscard]] audio::AudioEventAsset& asset() noexcept { return asset_; }
    [[nodiscard]] const audio::AudioEventCompileReport& report() const noexcept { return report_; }
    [[nodiscard]] std::size_t selected_node() const noexcept { return selectedNode_; }
    [[nodiscard]] bool search_active() const noexcept { return searchActive_; }
    [[nodiscard]] std::string_view palette_filter() const noexcept { return paletteFilter_; }
    [[nodiscard]] std::string_view status_message() const noexcept { return statusMessage_; }
    [[nodiscard]] bool can_undo() const noexcept { return historyCursor_ > 0U; }
    [[nodiscard]] bool can_redo() const noexcept { return historyCursor_ + 1U < history_.size(); }
    [[nodiscard]] bool connecting() const noexcept { return interaction_ == Interaction::Connect; }
    [[nodiscard]] std::optional<std::size_t> connection_source() const noexcept {
        return connecting() ? std::optional<std::size_t>(interactionNode_) : std::nullopt;
    }
    [[nodiscard]] int pointer_x() const noexcept { return pointerX_; }
    [[nodiscard]] int pointer_y() const noexcept { return pointerY_; }

    void resize(int width, int height, float uiScale = 1.0F) noexcept;
    bool pointer_down(int x, int y, audio::AudioMixer& mixer) noexcept;
    bool pointer_move(int x, int y) noexcept;
    bool pointer_up(int x, int y) noexcept;
    bool key_down(std::string_view key, bool control, bool shift, bool alt) noexcept;
    bool text_input(std::string_view text) noexcept;

    void compile();
    bool add_node(audio::AudioEventNodeType type, float x, float y);
    bool connect_nodes(std::size_t parent, std::size_t child);
    bool delete_selected();
    bool copy_selected();
    bool paste();
    bool undo();
    bool redo();
    [[nodiscard]] std::vector<AudioEventPropertyView> selected_properties() const;
    bool adjust_selected_property(AudioEventNodeProperty property, int steps);
    bool save(const std::filesystem::path& path, std::string* error = nullptr);
    bool open_asset(const std::filesystem::path& path, std::string* error = nullptr);
    void set_default_path(std::filesystem::path path) { defaultPath_ = std::move(path); }
    [[nodiscard]] const std::filesystem::path& default_path() const noexcept { return defaultPath_; }

private:
    enum class Interaction : std::uint8_t { Inactive, MoveNode, Connect };
    struct Clipboard {
        std::vector<audio::AudioEventNode> nodes;
        std::vector<audio::AudioEventEditorNode> positions;
        std::size_t root{};
        bool valid{};
    };

    void ensure_editor_positions();
    void rebuild_node_layout() noexcept;
    void rebuild_palette_layout() noexcept;
    void audition(audio::AudioMixer& mixer) noexcept;
    void commit_history();
    void restore_history(std::size_t index);
    void set_status(std::string message) { statusMessage_ = std::move(message); }
    [[nodiscard]] std::optional<std::size_t> node_at(int x, int y) const noexcept;
    [[nodiscard]] std::optional<std::size_t> input_port_at(int x, int y) const noexcept;
    [[nodiscard]] std::optional<std::size_t> output_port_at(int x, int y) const noexcept;
    [[nodiscard]] std::optional<audio::AudioEventNodeType> palette_type_at(int x, int y) const noexcept;
    [[nodiscard]] bool would_create_cycle(std::size_t parent, std::size_t child) const noexcept;
    [[nodiscard]] static std::size_t maximum_children(audio::AudioEventNodeType type) noexcept;
    [[nodiscard]] std::vector<std::size_t> selected_subgraph() const;

    AudioEventPanelLayout layout_{};
    audio::AudioEventAsset asset_{};
    audio::AudioEventCompileReport report_{};
    std::size_t selectedNode_{};
    bool open_{};
    bool searchActive_{};
    std::string paletteFilter_;
    std::string statusMessage_;
    std::filesystem::path defaultPath_{"editor_preview.dveaudio"};

    Interaction interaction_{Interaction::Inactive};
    std::size_t interactionNode_{};
    int pointerX_{};
    int pointerY_{};
    int dragOffsetX_{};
    int dragOffsetY_{};
    audio::AudioEventEditorNode dragStartPosition_{};
    bool dragChanged_{};

    Clipboard clipboard_{};
    std::vector<audio::AudioEventAsset> history_;
    std::size_t historyCursor_{};
};

} // namespace dve::editor
