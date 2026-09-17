#pragma once

#include <cstdint>

#include "dve/render/texture_residency.hpp"

namespace dve::render {

inline constexpr const char* kTexturePreviewShaderName = "Engine/Shaders/TexturePreview";

enum class TexturePreviewMode : std::uint32_t {
    ColorSrgb = 0,
    LinearData = 1,
    TangentNormal = 2,
    Alpha = 3,
    SingleChannel = 4,
    HdrColor = 5,
};

struct TexturePreviewSettings {
    TexturePreviewMode mode{TexturePreviewMode::ColorSrgb};
    std::uint32_t channel{};
    float exposure{1.0F};
    float checkerScale{12.0F};
    float mipLevel{};
    std::uint32_t arrayLayer{};
    float uvScaleX{1.0F};
    float uvScaleY{1.0F};
    float uvOffsetX{};
    float uvOffsetY{};
    bool falseColor{};
    bool nearestFiltering{};
};

// Selects the non-lighting preview mode appropriate for the texture's semantic. Base-color and
// emissive assets are treated as color; every packed/data texture is kept linear.
[[nodiscard]] TexturePreviewSettings default_texture_preview_settings(TextureSemantic semantic) noexcept;

// Deterministic engine fallbacks. Missing color is intentionally conspicuous; data fallbacks are
// neutral values that do not distort the material when a channel is optional.
[[nodiscard]] PolygonImage make_missing_texture_fallback(
    TextureSemantic semantic,
    std::uint32_t dimension = 8U);

} // namespace dve::render
