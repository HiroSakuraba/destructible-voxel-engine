#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dve {

constexpr std::size_t kMaximumSpritePaletteEntries = 256U;
constexpr std::uint32_t kSpritePaletteClockTicksPerSecond = 240U;
constexpr std::size_t kInvalidSpritePalettePacket = static_cast<std::size_t>(-1);

struct SpriteColor8 {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{255U};

    friend bool operator==(const SpriteColor8&, const SpriteColor8&) = default;
};

struct SpritePaletteBank {
    std::string name;
    std::vector<SpriteColor8> colors;
};

enum class SpritePaletteCycleDirection : std::uint8_t {
    Forward,
    Reverse,
    PingPong,
};

struct SpritePaletteCycleTrack {
    std::string name;
    std::uint16_t firstIndex{};
    std::uint16_t lastIndex{}; // inclusive
    std::uint32_t ticksPerStep{1U};
    std::int32_t phaseSteps{};
    SpritePaletteCycleDirection direction{SpritePaletteCycleDirection::Forward};
};

struct SpritePaletteAsset {
    std::string name;
    std::optional<std::uint16_t> transparentIndex;
    std::vector<SpritePaletteBank> banks;
    std::vector<SpritePaletteCycleTrack> cycles;
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
    [[nodiscard]] std::size_t entry_count() const noexcept {
        return banks.empty() ? 0U : banks.front().colors.size();
    }
};

[[nodiscard]] std::uint64_t sprite_palette_content_hash(
    const SpritePaletteAsset& palette) noexcept;
[[nodiscard]] const SpritePaletteBank* find_sprite_palette_bank(
    const SpritePaletteAsset& palette, std::string_view name) noexcept;

struct SpritePaletteReadResult {
    SpritePaletteAsset asset;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] bool write_dvepalette(
    const std::filesystem::path& path,
    const SpritePaletteAsset& palette,
    std::string* error = nullptr);
[[nodiscard]] SpritePaletteReadResult read_dvepalette(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = 4ULL * 1024ULL * 1024ULL);

struct SpritePaletteSwap {
    std::uint16_t sourceIndex{};
    std::uint16_t destinationIndex{};

    friend bool operator==(const SpritePaletteSwap&, const SpritePaletteSwap&) = default;
};

struct SpritePaletteResolveDesc {
    std::uint32_t bank{};
    std::uint64_t clockTicks{};
    std::span<const SpritePaletteSwap> swaps;
};

struct SpritePalettePacket {
    std::string paletteAsset;
    std::uint32_t bank{};
    std::uint64_t clockTicks{};
    std::uint64_t paletteContentHash{};
    std::uint64_t stateHash{};
    std::vector<SpriteColor8> colors;
};

[[nodiscard]] bool validate_sprite_palette_swaps(
    std::span<const SpritePaletteSwap> swaps,
    std::size_t entryCount,
    std::string* error = nullptr);
[[nodiscard]] bool validate_sprite_palette_swaps(
    const SpritePaletteAsset& palette,
    std::span<const SpritePaletteSwap> swaps,
    std::string* error = nullptr);
[[nodiscard]] std::uint32_t sample_sprite_palette_cycle_shift(
    const SpritePaletteCycleTrack& track,
    std::uint64_t clockTicks) noexcept;
[[nodiscard]] bool resolve_sprite_palette(
    const SpritePaletteAsset& palette,
    const SpritePaletteResolveDesc& desc,
    SpritePalettePacket& out,
    std::string* error = nullptr);

enum class SpritePaletteIndexPolicy : std::uint8_t {
    Exact,
    Nearest,
    Error,
};

struct SpritePaletteIndexSettings {
    SpritePaletteIndexPolicy policy{SpritePaletteIndexPolicy::Error};
    std::uint32_t bank{};
    std::uint8_t alphaThreshold{};
    std::uint16_t unmatchedIndex{};
};

struct SpritePaletteUnmatchedColor {
    SpriteColor8 color{};
    std::uint64_t pixels{};
};

struct SpritePaletteIndexDiagnostics {
    std::uint64_t totalPixels{};
    std::uint64_t transparentPixels{};
    std::uint64_t exactPixels{};
    std::uint64_t nearestPixels{};
    std::uint64_t unmatchedPixels{};
    std::uint32_t maximumDistanceSquared{};
    std::vector<SpritePaletteUnmatchedColor> unmatchedColors;
};

// Converts a top-left RGBA8 image to one byte per palette index. Exact maps unmatched colors to
// unmatchedIndex and reports them, Nearest chooses the deterministic least-distance entry, and
// Error rejects the conversion transactionally when any opaque source color is absent.
[[nodiscard]] bool index_sprite_rgba8(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::byte> rgba8,
    const SpritePaletteAsset& palette,
    const SpritePaletteIndexSettings& settings,
    std::vector<std::byte>& outIndices,
    SpritePaletteIndexDiagnostics& diagnostics,
    std::string* error = nullptr);

// Encodes one-byte indices into an RGBA8 transport image. R stores the index, G and B are zero,
// and A is zero only for the palette's declared transparent index. This is renderer-neutral and
// keeps the source atlas at one logical index per texel.
[[nodiscard]] bool encode_sprite_indices_rgba8(
    std::span<const std::byte> indices,
    const SpritePaletteAsset& palette,
    std::vector<std::byte>& outRgba8,
    std::string* error = nullptr);

} // namespace dve
