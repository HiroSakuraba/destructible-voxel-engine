#pragma once
// 8-bit frames produced by the player's renderers: hashing (deterministic smoke tests),
// PNG/PPM output (screenshots, references) and tolerant comparison (for goldens that may
// drift between compilers). No platform or image-library dependencies.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dve::player {

struct Rgba8Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels; // width * height * 4, row-major, top row first

    [[nodiscard]] bool valid() const noexcept {
        return width > 0U && height > 0U &&
               pixels.size() == static_cast<std::size_t>(width) * height * 4U;
    }
};

// FNV-1a 64 over the little-endian width, height and the RGBA bytes.
[[nodiscard]] std::uint64_t hash_image(const Rgba8Image& image) noexcept;
// 16 lowercase hex digits.
[[nodiscard]] std::string format_image_hash(std::uint64_t hash);

// Uncompressed (stored-deflate) RGBA PNG; readable by every viewer, no zlib needed.
[[nodiscard]] bool write_png(const std::filesystem::path& path, const Rgba8Image& image,
                             std::string* error = nullptr);
// Binary P6 PPM (alpha dropped).
[[nodiscard]] bool write_ppm(const std::filesystem::path& path, const Rgba8Image& image,
                             std::string* error = nullptr);
[[nodiscard]] std::optional<Rgba8Image> read_ppm(const std::filesystem::path& path,
                                                 std::string* error = nullptr);

// Box-filters by an integer factor (dimensions must be divisible by it).
[[nodiscard]] std::optional<Rgba8Image> downsample_box(const Rgba8Image& image, std::uint32_t factor);

struct ImageDifference {
    std::uint32_t maxChannelDelta{};
    double meanAbsoluteDelta{};      // over RGB channels, 0..255
    std::uint64_t pixelsOverThreshold{};
    std::uint64_t pixelCount{};
};
// RGB comparison of two images with the same size; nullopt when sizes differ.
[[nodiscard]] std::optional<ImageDifference> compare_images(
    const Rgba8Image& a, const Rgba8Image& b, std::uint32_t perChannelThreshold);

} // namespace dve::player
