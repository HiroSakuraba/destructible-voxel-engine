#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/sprite2d.hpp"

namespace dve::editor {

struct SpriteSourceImageView {
    std::uint32_t width{};
    std::uint32_t height{};
    std::span<const std::byte> rgba8;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct SpriteSliceSpec {
    std::string name;
    SpriteRectPixels rect{};
    SpriteVec2 pivotPixels{}; // untrimmed bottom-left coordinates
    float durationSeconds{1.0F / 12.0F};
    std::string event;
};

struct SpriteGridSliceSettings {
    std::uint32_t originX{};
    std::uint32_t originY{};
    std::uint32_t cellWidth{16U};
    std::uint32_t cellHeight{16U};
    std::uint32_t spacingX{};
    std::uint32_t spacingY{};
    std::uint32_t columns{}; // zero derives every complete cell
    std::uint32_t rows{};
    std::string frameNamePrefix{"frame"};
    SpriteVec2 pivotPixels{8.0F, 0.0F};
    float durationSeconds{1.0F / 12.0F};
    std::uint8_t alphaThreshold{};
    bool trimTransparent{true};
    bool skipTransparent{true};
};

enum class SpriteTrackItemKind : std::uint8_t { CombatWindow, SocketKey, PropertyKey, RootMotionKey };

struct SpriteTrackSelection {
    SpriteTrackItemKind kind{SpriteTrackItemKind::CombatWindow};
    SpriteTrackId id{kInvalidSpriteTrackId};
};

struct SpriteAuthoringDocument {
    SpriteAsset asset;
    std::string selectedClip;
    std::optional<SpriteFrameIndex> selectedFrame;
    std::optional<SpriteTrackSelection> selectedTrack;
    std::uint64_t revision{};
};

struct SpriteAuthoringPreview {
    SpriteSample sample;
    SpriteFrame frame;
    SpriteRenderList renderList;
    std::string clip;
    float timeSeconds{};
    bool playing{};
};

[[nodiscard]] SpriteAsset make_sprite_authoring_asset(
    std::string name,
    std::string textureAsset,
    std::uint32_t textureWidth,
    std::uint32_t textureHeight,
    float pixelsPerWorldUnit = 16.0F);

class SpriteAuthoringSession {
public:
    explicit SpriteAuthoringSession(SpriteAsset asset = make_sprite_authoring_asset(
        "Untitled Sprite", "textures/untitled.png", 1U, 1U, 16.0F));

    [[nodiscard]] const SpriteAuthoringDocument& document() const noexcept { return document_; }
    [[nodiscard]] const SpriteAsset& asset() const noexcept { return document_.asset; }
    [[nodiscard]] bool dirty() const noexcept { return document_.asset.contentHash != savedHash_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] bool open(const std::filesystem::path& path, std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string* error = nullptr);

    // Replaces the complete authored asset as one validated undo transaction. Production
    // range tools use this when one operation changes clip tracks and frame-level events together.
    [[nodiscard]] bool replace_asset(
        SpriteAsset replacement, std::string label = "Replace sprite asset",
        std::string* error = nullptr);

    [[nodiscard]] bool slice_grid(
        const SpriteSourceImageView& image,
        const SpriteGridSliceSettings& settings,
        std::string* error = nullptr);
    [[nodiscard]] bool replace_slices(
        const SpriteSourceImageView& image,
        std::span<const SpriteSliceSpec> slices,
        bool trimTransparent = true,
        std::uint8_t alphaThreshold = 0U,
        bool skipTransparent = true,
        std::string* error = nullptr);

    [[nodiscard]] bool update_frame(
        SpriteFrameIndex frame, SpriteFrame replacement, std::string* error = nullptr);
    [[nodiscard]] bool set_frame_pivot(
        SpriteFrameIndex frame, SpriteVec2 pivotPixels, std::string* error = nullptr);
    [[nodiscard]] bool set_frame_timing_event(
        SpriteFrameIndex frame, float durationSeconds, std::string event,
        std::string* error = nullptr);
    [[nodiscard]] bool set_atlas_layout(
        std::uint32_t textureWidth, std::uint32_t textureHeight,
        std::span<const SpriteRectPixels> frameRects,
        std::string* error = nullptr);
    [[nodiscard]] bool set_texture_source(
        std::string textureAsset, std::uint32_t textureWidth, std::uint32_t textureHeight,
        std::string* error = nullptr);
    [[nodiscard]] bool set_palette_source(
        std::string paletteAsset, std::string* error = nullptr);

    [[nodiscard]] bool add_clip(
        SpriteClip clip, std::string* error = nullptr);
    [[nodiscard]] bool update_clip(
        std::string_view clipName, SpriteClip replacement, std::string* error = nullptr);
    [[nodiscard]] bool remove_clip(std::string_view clipName, std::string* error = nullptr);
    [[nodiscard]] bool move_clip_frame(
        std::string_view clipName, std::size_t from, std::size_t to,
        std::string* error = nullptr);
    [[nodiscard]] bool reorder_clip_timeline(
        std::string_view clipName, std::span<const std::size_t> oldIndicesInNewOrder,
        std::string* error = nullptr);

    [[nodiscard]] bool add_combat_window(
        std::string_view clipName, SpriteCombatWindow window,
        std::string* error = nullptr);
    [[nodiscard]] bool update_combat_window(
        std::string_view clipName, std::size_t windowIndex, SpriteCombatWindow replacement,
        std::string* error = nullptr);
    [[nodiscard]] bool remove_combat_window(
        std::string_view clipName, std::size_t windowIndex,
        std::string* error = nullptr);
    [[nodiscard]] bool upsert_socket_key(
        std::string_view clipName, SpriteSocketKey key,
        std::string* error = nullptr);
    [[nodiscard]] bool remove_socket_key(
        std::string_view clipName, std::string_view socketName, std::size_t sequenceIndex,
        std::string* error = nullptr);
    [[nodiscard]] bool upsert_property_key(
        std::string_view clipName, SpritePropertyKey key,
        std::string* error = nullptr);
    [[nodiscard]] bool remove_property_key(
        std::string_view clipName, std::string_view propertyName, std::size_t sequenceIndex,
        std::string* error = nullptr);
    [[nodiscard]] bool set_root_motion_key(
        std::string_view clipName, SpriteRootMotionKey key,
        std::string* error = nullptr);
    [[nodiscard]] bool remove_root_motion_key(
        std::string_view clipName, std::size_t sequenceIndex,
        std::string* error = nullptr);

    [[nodiscard]] bool select_track(SpriteTrackItemKind kind, SpriteTrackId id) noexcept;
    void clear_track_selection() noexcept { document_.selectedTrack.reset(); }
    [[nodiscard]] bool update_combat_window_by_id(
        std::string_view clipName, SpriteTrackId id, SpriteCombatWindow replacement,
        std::string* error = nullptr);
    [[nodiscard]] bool remove_track_by_id(
        std::string_view clipName, SpriteTrackItemKind kind, SpriteTrackId id,
        std::string* error = nullptr);
    [[nodiscard]] bool translate_combat_window(
        std::string_view clipName, SpriteTrackId id, SpriteVec2 deltaPixels,
        std::string* error = nullptr);
    [[nodiscard]] bool resize_combat_window(
        std::string_view clipName, SpriteTrackId id, SpriteVec2 sizePixels,
        float radiusPixels, std::string* error = nullptr);
    [[nodiscard]] bool rotate_combat_window(
        std::string_view clipName, SpriteTrackId id, float rotationDegrees,
        std::string* error = nullptr);
    [[nodiscard]] bool set_combat_window_range(
        std::string_view clipName, SpriteTrackId id, std::size_t firstSequenceIndex,
        std::size_t lastSequenceIndex, std::string* error = nullptr);
    [[nodiscard]] bool update_socket_key_by_id(
        std::string_view clipName, SpriteTrackId id, SpriteSocketKey replacement,
        std::string* error = nullptr);
    [[nodiscard]] bool update_property_key_by_id(
        std::string_view clipName, SpriteTrackId id, SpritePropertyKey replacement,
        std::string* error = nullptr);
    [[nodiscard]] bool update_root_motion_key_by_id(
        std::string_view clipName, SpriteTrackId id, SpriteRootMotionKey replacement,
        std::string* error = nullptr);
    [[nodiscard]] bool duplicate_track_item(
        std::string_view clipName, SpriteTrackItemKind kind, SpriteTrackId id,
        std::ptrdiff_t sequenceOffset, SpriteTrackId* duplicateId = nullptr,
        std::string* error = nullptr);
    [[nodiscard]] bool mirror_track_item_x(
        std::string_view clipName, SpriteTrackItemKind kind, SpriteTrackId id,
        std::string* error = nullptr);

    [[nodiscard]] bool select_clip(std::string_view clipName) noexcept;
    [[nodiscard]] bool select_frame(SpriteFrameIndex frame) noexcept;
    [[nodiscard]] bool set_preview_playing(bool playing) noexcept;
    [[nodiscard]] bool seek_preview(float timeSeconds) noexcept;
    [[nodiscard]] bool tick_preview(float deltaSeconds) noexcept;
    [[nodiscard]] std::optional<SpriteAuthoringPreview> preview(
        std::string* error = nullptr) const;

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::string_view undo_label() const noexcept;
    [[nodiscard]] std::string_view redo_label() const noexcept;
    [[nodiscard]] bool undo() noexcept;
    [[nodiscard]] bool redo() noexcept;
    void clear_history() noexcept;

private:
    struct HistoryEntry {
        std::string label;
        SpriteAuthoringDocument state;
    };

    [[nodiscard]] bool mutate(
        std::string label,
        const std::function<bool(SpriteAsset&, std::string*)>& operation,
        std::string* error);
    void normalize_selection() noexcept;

    SpriteAuthoringDocument document_;
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
    std::size_t historyLimit_{128U};
    std::filesystem::path path_;
    std::uint64_t savedHash_{};
    float previewTimeSeconds_{};
    bool previewPlaying_{true};
};

} // namespace dve::editor
