#include "dve/render/texture_preview.hpp"

#include <algorithm>

namespace dve::render {
namespace {
void write_pixel(PolygonImage& image, std::uint32_t x, std::uint32_t y,
                 std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255U) {
    const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
    image.rgba8[offset] = r;
    image.rgba8[offset + 1U] = g;
    image.rgba8[offset + 2U] = b;
    image.rgba8[offset + 3U] = a;
}
} // namespace

TexturePreviewSettings default_texture_preview_settings(TextureSemantic semantic) noexcept {
    TexturePreviewSettings settings;
    switch (semantic) {
        case TextureSemantic::BaseColor:
        case TextureSemantic::Emissive:
            settings.mode = TexturePreviewMode::ColorSrgb;
            break;
        case TextureSemantic::Normal:
            settings.mode = TexturePreviewMode::TangentNormal;
            break;
        case TextureSemantic::Opacity:
            settings.mode = TexturePreviewMode::Alpha;
            settings.channel = 3U;
            break;
        case TextureSemantic::MetallicRoughness:
        case TextureSemantic::AmbientOcclusion:
            settings.mode = TexturePreviewMode::LinearData;
            break;
    }
    return settings;
}

PolygonImage make_missing_texture_fallback(TextureSemantic semantic, std::uint32_t dimension) {
    PolygonImage image;
    image.name = "DVE Missing Texture Fallback";
    image.mimeType = "image/raw-rgba8";
    image.width = std::clamp(dimension, 2U, 256U);
    image.height = image.width;
    image.rgba8.resize(static_cast<std::size_t>(image.width) * image.height * 4U);

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            switch (semantic) {
                case TextureSemantic::BaseColor: {
                    const std::uint32_t cell = std::max(1U, image.width / 4U);
                    const bool magenta = ((x / cell) + (y / cell)) % 2U == 0U;
                    write_pixel(image, x, y, magenta ? 255U : 0U, 0U, magenta ? 255U : 0U);
                    break;
                }
                case TextureSemantic::Normal:
                    write_pixel(image, x, y, 128U, 128U, 255U);
                    break;
                case TextureSemantic::MetallicRoughness:
                    // glTF-style packing: G=roughness, B=metallic. R remains neutral/unused.
                    write_pixel(image, x, y, 255U, 128U, 0U);
                    break;
                case TextureSemantic::AmbientOcclusion:
                case TextureSemantic::Opacity:
                    write_pixel(image, x, y, 255U, 255U, 255U, 255U);
                    break;
                case TextureSemantic::Emissive:
                    write_pixel(image, x, y, 0U, 0U, 0U, 255U);
                    break;
            }
        }
    }
    return image;
}

} // namespace dve::render
