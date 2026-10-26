#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace dve {
// Dependency-free, top-left-origin RGBA8 PNG output, shared by tools and the player.
// Channel interpretation is supplied by the material binding/export manifest.
[[nodiscard]] bool write_rgba8_png(const std::filesystem::path&, std::uint32_t width,
    std::uint32_t height, std::span<const std::uint8_t> pixels, std::string* error=nullptr);
}
