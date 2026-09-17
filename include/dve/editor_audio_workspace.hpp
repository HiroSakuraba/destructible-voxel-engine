#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "dve/audio/audio_analysis.hpp"
#include "dve/audio/audio_edit.hpp"
#include "dve/editor_audio_timeline.hpp"
#include "dve/editor_native_renderer.hpp"

namespace dve::editor {

enum class AudioEditorTool : std::uint8_t {
    Select, Move, Trim, Split, Slip, Ripple, Roll, Crossfade, Scrub, Shuttle, Zoom
};

struct AudioEditorCommandDescriptor {
    std::string_view id;
    std::string_view name;
    std::string_view defaultShortcut;
};

[[nodiscard]] std::span<const AudioEditorCommandDescriptor> audio_editor_commands() noexcept;
[[nodiscard]] std::string_view audio_editor_tool_name(AudioEditorTool tool) noexcept;

struct AudioTimelineSelection {
    std::uint64_t beginFrame{};
    std::uint64_t endFrame{};
    bool active{};
};

class AudioTimelineEditorController {
public:
    AudioTimelineEditorController(audio::AudioEditSession& session,
                                  audio::AudioEditHistory& history,
                                  audio::AudioEditTransport& transport,
                                  AudioTimelineView view = {});

    [[nodiscard]] AudioTimelineView& view() noexcept { return view_; }
    [[nodiscard]] const AudioTimelineView& view() const noexcept { return view_; }
    [[nodiscard]] AudioEditorTool tool() const noexcept { return tool_; }
    [[nodiscard]] std::optional<audio::AudioEditClipId> selected_clip() const noexcept { return selectedClip_; }
    [[nodiscard]] AudioTimelineSelection selection() const noexcept { return selection_; }
    void set_tool(AudioEditorTool tool) noexcept { tool_ = tool; }
    void set_content_rect(UiRect content) noexcept { content_ = content; }
    void set_peak_caches(const std::vector<AudioTimelinePeakCacheEntry>* caches) noexcept { caches_ = caches; }
    void set_spectrogram_caches(const std::vector<AudioTimelineSpectrogramCacheEntry>* caches) noexcept { spectrograms_ = caches; }

    bool pointer_down(int x, int y, bool extendSelection = false);
    bool pointer_move(int x, int y);
    bool pointer_up(int x, int y);
    bool wheel(float delta, int x, bool controlModifier = false);
    bool dispatch_command(std::string_view command);

    [[nodiscard]] AudioTimelineDrawModel draw_model() const;
private:
    enum class DragMode : std::uint8_t { Inactive, SelectRange, Move, TrimLeft, TrimRight, Slip, Scrub };
    audio::AudioEditSession& session_;
    audio::AudioEditHistory& history_;
    audio::AudioEditTransport& transport_;
    AudioTimelineView view_;
    UiRect content_{};
    const std::vector<AudioTimelinePeakCacheEntry>* caches_{};
    const std::vector<AudioTimelineSpectrogramCacheEntry>* spectrograms_{};
    AudioEditorTool tool_{AudioEditorTool::Select};
    std::optional<audio::AudioEditClipId> selectedClip_;
    std::optional<audio::AudioEditClipId> dragOtherClip_;
    AudioTimelineSelection selection_{};
    DragMode dragMode_{DragMode::Inactive};
    int dragStartX_{};
    std::uint64_t dragStartFrame_{};
    std::uint64_t originalTimelineStart_{};
    std::uint64_t originalSourceStart_{};
    std::uint64_t originalSourceCount_{};
    bool transactionChanged_{};
};

struct AudioTimelineRenderStyle {
    EditorColor background{0x0A1018U};
    EditorColor panel{0x111B28U};
    EditorColor gridMinor{0x1A2A3AU};
    EditorColor gridMajor{0x29445FU};
    EditorColor clip{0x285A7AU};
    EditorColor clipSelected{0x2F83B5U};
    EditorColor waveform{0xA4D7F5U};
    EditorColor marker{0xF2C14EU};
    EditorColor playhead{0xFF5964U};
    EditorColor loop{0x264D3AU};
    EditorColor punch{0x573A31U};
    EditorColor text{0xE8F0F8U};
    EditorColor mutedText{0x91A0AFU};
    EditorColor comp{0x73D2A6U};
};

void render_audio_timeline_workspace(const IEditorCanvas& canvas,
                                     const audio::AudioEditSession& session,
                                     const AudioTimelineDrawModel& model,
                                     const AudioTimelineView& view,
                                     AudioEditorTool tool,
                                     AudioTimelineSelection selection = {},
                                     const audio::AudioAnalysisReport* analysis = nullptr,
                                     const AudioTimelineRenderStyle& style = {});

} // namespace dve::editor
