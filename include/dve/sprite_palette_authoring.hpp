#pragma once

#include "dve/sprite_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dve {

struct SpritePaletteAuthoringSelection {
    std::size_t bank{};
    std::size_t color{};
    std::optional<std::size_t> cycle;
};

enum class SpritePaletteExternalChange : std::uint8_t {
    Unchanged,
    Reloaded,
    Conflict,
    Missing,
    Failed,
};

struct SpritePaletteExternalChangeResult {
    SpritePaletteExternalChange state{SpritePaletteExternalChange::Unchanged};
    std::string message;
};

struct SpritePaletteImportResult {
    std::size_t uniqueColors{};
    std::uint64_t transparentPixels{};
    std::uint64_t discardedColors{};
};

// Transactional authoring document for native and headless palette tools. Every successful edit
// validates the complete palette before commit and records a bounded undo snapshot.
class SpritePaletteAuthoringSession {
public:
    SpritePaletteAuthoringSession();
    explicit SpritePaletteAuthoringSession(SpritePaletteAsset asset);

    [[nodiscard]] const SpritePaletteAsset& asset() const noexcept { return asset_; }
    [[nodiscard]] SpritePaletteAuthoringSelection selection() const noexcept { return selection_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] bool preview_playing() const noexcept { return previewPlaying_; }
    [[nodiscard]] std::uint64_t preview_ticks() const noexcept { return previewTicks_; }

    [[nodiscard]] bool create(std::string name, std::size_t entries = 16U,
                              std::string* error = nullptr);
    [[nodiscard]] bool open(const std::filesystem::path& path,
                            std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path = {},
                            std::string* error = nullptr);

    [[nodiscard]] bool select_bank(std::size_t index) noexcept;
    [[nodiscard]] bool select_color(std::size_t index) noexcept;
    [[nodiscard]] bool select_cycle(std::optional<std::size_t> index) noexcept;

    [[nodiscard]] bool set_color(std::size_t index, SpriteColor8 color,
                                 std::string* error = nullptr);
    [[nodiscard]] bool set_selected_color(SpriteColor8 color,
                                          std::string* error = nullptr);
    [[nodiscard]] bool add_bank(std::string name, bool duplicateSelected = true,
                                std::string* error = nullptr);
    [[nodiscard]] bool rename_bank(std::size_t index, std::string name,
                                   std::string* error = nullptr);
    [[nodiscard]] bool remove_bank(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool set_transparent_index(std::optional<std::uint16_t> index,
                                             std::string* error = nullptr);
    [[nodiscard]] bool add_cycle(SpritePaletteCycleTrack track,
                                 std::string* error = nullptr);
    [[nodiscard]] bool update_cycle(std::size_t index, SpritePaletteCycleTrack track,
                                    std::string* error = nullptr);
    [[nodiscard]] bool remove_cycle(std::size_t index, std::string* error = nullptr);

    // Replaces the palette with deterministic row-major unique colors from an RGBA8 image.
    // Transparent pixels collapse to RGBA(0,0,0,0) at index zero when present.
    [[nodiscard]] bool import_rgba8(std::string name, std::uint32_t width, std::uint32_t height,
                                    std::span<const std::byte> rgba8,
                                    std::uint8_t alphaThreshold,
                                    SpritePaletteImportResult& result,
                                    std::string* error = nullptr);

    void set_preview_playing(bool playing) noexcept { previewPlaying_ = playing; }
    void seek_preview(std::uint64_t ticks) noexcept { previewTicks_ = ticks; }
    void advance_preview(std::uint64_t ticks) noexcept;
    [[nodiscard]] bool resolve_preview(SpritePalettePacket& packet,
                                       std::string* error = nullptr) const;

    [[nodiscard]] bool undo(std::string* error = nullptr);
    [[nodiscard]] bool redo(std::string* error = nullptr);
    [[nodiscard]] SpritePaletteExternalChangeResult poll_external_change(bool force = false);

private:
    struct Snapshot {
        SpritePaletteAsset asset;
        SpritePaletteAuthoringSelection selection;
    };

    template <class Edit>
    [[nodiscard]] bool apply_edit(Edit&& edit, std::string* error);
    void normalize_selection() noexcept;
    void remember_signature() noexcept;

    SpritePaletteAsset asset_;
    SpritePaletteAuthoringSelection selection_{};
    std::filesystem::path path_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    bool dirty_{};
    bool previewPlaying_{true};
    std::uint64_t previewTicks_{};
    std::filesystem::file_time_type writeTime_{};
    std::uintmax_t fileSize_{};
    bool signatureValid_{};
};

} // namespace dve
