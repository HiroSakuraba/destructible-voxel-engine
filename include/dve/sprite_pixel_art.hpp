#pragma once

#include <array>
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
#include "dve/sprite_palette.hpp"

namespace dve::editor {

enum class SpritePixelStorage : std::uint8_t { Indexed8, Rgba8 };
enum class SpritePixelBlendMode : std::uint8_t { Normal, Add, Multiply };
enum class SpritePixelTool : std::uint8_t {
    Pencil,
    Eraser,
    Fill,
    Line,
    Rectangle,
    Selection,
    MoveSelection,
};

struct SpritePixelRect {
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] bool contains(std::int32_t px, std::int32_t py) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return width == 0U || height == 0U; }
};

struct SpritePixelLayer {
    std::uint64_t id{};
    std::string name;
    bool visible{true};
    bool locked{};
    std::uint8_t opacity{255U};
    SpritePixelBlendMode blend{SpritePixelBlendMode::Normal};
    // Indexed8 uses one byte per texel. Rgba8 uses four bytes per texel.
    std::vector<std::byte> pixels;
};

struct SpritePixelFrame {
    std::string name;
    float durationSeconds{1.0F / 12.0F};
    std::vector<SpritePixelLayer> layers;
};

struct SpritePixelArtDocument {
    std::string name{"Untitled Pixel Art"};
    SpritePixelStorage storage{SpritePixelStorage::Indexed8};
    std::uint32_t width{16U};
    std::uint32_t height{16U};
    SpritePaletteAsset palette;
    std::vector<SpritePixelFrame> frames;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
};

struct SpritePixelArtPublishSettings {
    std::filesystem::path imagePath;
    std::filesystem::path spritePath;
    std::filesystem::path palettePath;
    std::string imageAssetReference;
    std::string paletteAssetReference;
    std::string clipName{"default"};
    float pixelsPerWorldUnit{16.0F};
    std::uint32_t columns{}; // zero chooses a near-square sheet
    bool indexedTransport{true};
};

struct SpritePixelArtPublishResult {
    SpriteAsset sprite;
    std::uint32_t sheetWidth{};
    std::uint32_t sheetHeight{};
    std::uint64_t imageHash{};
    std::uint64_t documentHash{};
};

class SpritePixelArtSession {
public:
    explicit SpritePixelArtSession(SpritePixelArtDocument document = {});

    [[nodiscard]] const SpritePixelArtDocument& document() const noexcept { return document_; }
    [[nodiscard]] SpritePixelArtDocument& document_for_testing() noexcept { return document_; }
    [[nodiscard]] std::size_t selected_frame() const noexcept { return selectedFrame_; }
    [[nodiscard]] std::size_t selected_layer() const noexcept { return selectedLayer_; }
    [[nodiscard]] const std::optional<SpritePixelRect>& selection() const noexcept { return selection_; }
    [[nodiscard]] SpritePixelTool tool() const noexcept { return tool_; }
    [[nodiscard]] bool wrap_painting() const noexcept { return wrapPainting_; }
    [[nodiscard]] bool dirty() const noexcept { return document_.contentHash != savedHash_; }

    [[nodiscard]] bool new_document(std::string name, std::uint32_t width, std::uint32_t height,
                                    SpritePixelStorage storage, SpritePaletteAsset palette,
                                    std::string* error = nullptr);
    [[nodiscard]] bool open(const std::filesystem::path& path, std::string* error = nullptr);
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string* error = nullptr);

    [[nodiscard]] bool select_frame(std::size_t index) noexcept;
    [[nodiscard]] bool select_layer(std::size_t index) noexcept;
    void set_tool(SpritePixelTool tool) noexcept { tool_ = tool; }
    void set_wrap_painting(bool enabled) noexcept { wrapPainting_ = enabled; }
    void set_index(std::uint8_t index) noexcept { selectedIndex_ = index; }
    void set_color(SpriteColor8 color) noexcept { selectedColor_ = color; }

    [[nodiscard]] bool add_frame(std::string name, bool cloneCurrent = true,
                                 std::string* error = nullptr);
    [[nodiscard]] bool remove_frame(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool set_frame_duration(std::size_t index, float seconds,
                                          std::string* error = nullptr);
    [[nodiscard]] bool add_layer(std::string name, std::string* error = nullptr);
    [[nodiscard]] bool remove_layer(std::size_t index, std::string* error = nullptr);
    [[nodiscard]] bool move_layer(std::size_t from, std::size_t to, std::string* error = nullptr);
    [[nodiscard]] bool set_layer_visibility(std::size_t index, bool visible,
                                            std::string* error = nullptr);
    [[nodiscard]] bool set_layer_opacity(std::size_t index, std::uint8_t opacity,
                                         std::string* error = nullptr);

    [[nodiscard]] bool paint(std::int32_t x, std::int32_t y, std::string* error = nullptr);
    [[nodiscard]] bool erase(std::int32_t x, std::int32_t y, std::string* error = nullptr);
    [[nodiscard]] bool draw_line(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                                 std::string* error = nullptr);
    [[nodiscard]] bool draw_rectangle(SpritePixelRect rect, bool filled,
                                      std::string* error = nullptr);
    [[nodiscard]] bool flood_fill(std::int32_t x, std::int32_t y, std::string* error = nullptr);
    [[nodiscard]] bool set_selection(SpritePixelRect rect, std::string* error = nullptr);
    void clear_selection() noexcept { selection_.reset(); }
    [[nodiscard]] bool move_selection(std::int32_t dx, std::int32_t dy, bool copy,
                                      std::string* error = nullptr);
    [[nodiscard]] bool flip_selection_x(std::string* error = nullptr);
    [[nodiscard]] bool flip_selection_y(std::string* error = nullptr);
    [[nodiscard]] bool rotate_selection_90(bool clockwise, std::string* error = nullptr);
    [[nodiscard]] bool remap_index(std::uint8_t from, std::uint8_t to,
                                   bool allFrames, std::string* error = nullptr);

    [[nodiscard]] bool composite_rgba8(std::size_t frame, std::vector<std::byte>& out,
                                       std::string* error = nullptr) const;
    [[nodiscard]] bool composite_index8(std::size_t frame, std::vector<std::byte>& out,
                                        std::string* error = nullptr) const;
    [[nodiscard]] bool onion_rgba8(std::size_t frame, bool previous, bool next,
                                   std::vector<std::byte>& out,
                                   std::string* error = nullptr) const;
    [[nodiscard]] bool publish(const SpritePixelArtPublishSettings& settings,
                               SpritePixelArtPublishResult& out,
                               std::string* error = nullptr) const;

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] bool undo() noexcept;
    [[nodiscard]] bool redo() noexcept;

private:
    struct Snapshot {
        std::string label;
        SpritePixelArtDocument document;
        std::size_t frame{};
        std::size_t layer{};
        std::optional<SpritePixelRect> selection;
    };

    [[nodiscard]] std::size_t bytes_per_pixel() const noexcept;
    [[nodiscard]] SpritePixelLayer* current_layer() noexcept;
    [[nodiscard]] const SpritePixelLayer* current_layer() const noexcept;
    [[nodiscard]] bool mutate(std::string label, const std::function<bool(std::string*)>& operation,
                              std::string* error);
    [[nodiscard]] bool normalize_coordinate(std::int32_t& x, std::int32_t& y) const noexcept;
    [[nodiscard]] bool set_pixel(SpritePixelLayer& layer, std::int32_t x, std::int32_t y,
                                 bool erasePixel, std::string* error);
    void restore(const Snapshot& snapshot) noexcept;

    SpritePixelArtDocument document_;
    std::uint64_t savedHash_{};
    std::size_t selectedFrame_{};
    std::size_t selectedLayer_{};
    std::optional<SpritePixelRect> selection_;
    SpritePixelTool tool_{SpritePixelTool::Pencil};
    bool wrapPainting_{};
    std::uint8_t selectedIndex_{1U};
    SpriteColor8 selectedColor_{255U, 255U, 255U, 255U};
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
};

[[nodiscard]] bool write_dvepixel(const std::filesystem::path& path,
                                  const SpritePixelArtDocument& document,
                                  std::string* error = nullptr);
[[nodiscard]] bool read_dvepixel(const std::filesystem::path& path,
                                 SpritePixelArtDocument& document,
                                 std::string* error = nullptr,
                                 std::uint64_t maximumBytes = 256ULL * 1024ULL * 1024ULL);

} // namespace dve::editor
