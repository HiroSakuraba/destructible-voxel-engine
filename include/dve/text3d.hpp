#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/polygon_asset.hpp"

namespace dve {

// DVE's Slug integration keeps the original font outside the cooked asset. A cooked .dtext file
// contains only the glyph subset required by the authored string, the Slug curve/band data, and
// extrusion geometry. Font licensing remains the responsibility of the project author.

enum class Text3DHorizontalAlignment : std::uint8_t { Left, Center, Right };
enum class Text3DFillRule : std::uint8_t { NonZero, EvenOdd };

struct Text3DStyle {
    float emSizeMeters{1.0F};
    float extrusionDepthMeters{0.12F};
    float letterSpacingEm{};
    float lineSpacingEm{1.2F};
    std::uint16_t horizontalBands{8};
    std::uint16_t verticalBands{8};
    std::uint16_t curveSubdivision{8};
    Text3DHorizontalAlignment alignment{Text3DHorizontalAlignment::Left};
    Text3DFillRule fillRule{Text3DFillRule::NonZero};
    // Stable material identities are written into object/material-ID targets by the text renderer.
    std::uint32_t faceMaterialId{1};
    std::uint32_t sideMaterialId{2};
    Float4 faceColor{0.50F, 0.50F, 0.50F, 1.0F};
    Float4 sideColor{0.34F, 0.34F, 0.34F, 1.0F};
};

struct Text3DFontInfo {
    std::string family;
    std::string subfamily;
    std::uint16_t unitsPerEm{};
    std::int16_t ascender{};
    std::int16_t descender{};
    std::int16_t lineGap{};
    std::uint16_t glyphCount{};
    std::uint64_t sourceHash{};
};

struct Text3DQuadraticCurve {
    Float2 p1{};
    Float2 p2{};
    Float2 p3{};
};

struct Text3DContour {
    std::vector<Text3DQuadraticCurve> curves;
};

struct Text3DGlyphGeometry {
    std::uint32_t codepoint{};
    std::uint16_t glyphIndex{};
    float advanceEm{};
    float leftSideBearingEm{};
    Float2 minimumEm{};
    Float2 maximumEm{};
    std::vector<Text3DContour> contours;
};

struct SlugBandTexel {
    std::uint16_t x{};
    std::uint16_t y{};
};

struct SlugGlyphInstance {
    std::uint32_t codepoint{};
    std::uint16_t glyphIndex{};
    Float2 originMeters{};
    Float2 minimumEm{};
    Float2 maximumEm{};
    Float4 bandTransform{};
    std::array<std::int32_t, 4> glyphData{};
    Float4 color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct SlugTextFaceVertex {
    Float3 position{};
    Float2 dilationNormal{};
    Float2 renderCoordinate{};
    Float4 inverseJacobian{};
    Float4 bandTransform{};
    std::array<std::uint32_t, 4> glyphData{};
    Float4 color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct Text3DRenderPacket {
    std::vector<SlugTextFaceVertex> faceVertices;
    std::vector<std::uint32_t> faceIndices;
    std::uint64_t contentHash{};
};

struct SlugAtlas {
    static constexpr std::uint32_t kTextureWidth = 4096U;
    std::vector<Float4> curveTexels;
    std::vector<SlugBandTexel> bandTexels;
    std::uint32_t curveTextureHeight{};
    std::uint32_t bandTextureHeight{};
};

struct CookedText3DAsset {
    std::uint64_t objectId{1};
    std::string textUtf8;
    Text3DStyle style{};
    Text3DFontInfo font{};
    std::vector<Text3DGlyphGeometry> glyphGeometry;
    std::vector<SlugGlyphInstance> glyphInstances;
    SlugAtlas atlas{};
    CookedPolygonAsset sideMesh{};
    PolygonBounds bounds{};
    std::uint64_t contentHash{};
};

enum class Text3DErrorCode : std::uint8_t {
    NoError,
    Io,
    UnsupportedFont,
    CorruptFont,
    InvalidUtf8,
    MissingGlyph,
    LimitExceeded,
    InvalidStyle,
    InvalidAsset,
    UnsupportedVersion,
    HashMismatch,
};

struct Text3DCookOptions {
    std::uint64_t objectId{1};
    Text3DStyle style{};
    bool replaceMissingGlyphs{true};
    std::uint32_t replacementCodepoint{0xFFFDU};
    std::uint32_t maximumCodepoints{16384};
    std::uint32_t maximumUniqueGlyphs{4096};
    std::uint32_t maximumCurves{1U << 20U};
    std::uint64_t maximumFontBytes{256ULL * 1024ULL * 1024ULL};
};

struct Text3DCookResult {
    CookedText3DAsset asset{};
    Text3DErrorCode code{Text3DErrorCode::NoError};
    std::string error;
    std::vector<std::string> warnings;
    [[nodiscard]] explicit operator bool() const noexcept { return code == Text3DErrorCode::NoError; }
};

struct Text3DReadResult {
    CookedText3DAsset asset{};
    Text3DErrorCode code{Text3DErrorCode::NoError};
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return code == Text3DErrorCode::NoError; }
};

struct Text3DPreviewOptions {
    std::uint32_t width{960};
    std::uint32_t height{540};
    float pixelsPerMeter{180.0F};
    Float4 background{0.025F, 0.035F, 0.055F, 1.0F};
    Float3 lightDirection{0.35F, 0.45F, 0.82F};
    float extrusionScreenOffset{0.40F};
};

[[nodiscard]] Text3DCookResult cook_text3d(
    const std::filesystem::path& fontPath,
    std::string_view textUtf8,
    const Text3DCookOptions& options = {});

[[nodiscard]] std::uint64_t text3d_content_hash(const CookedText3DAsset& asset) noexcept;
[[nodiscard]] Text3DRenderPacket build_text3d_render_packet(const CookedText3DAsset& asset);
[[nodiscard]] bool validate_text3d_asset(const CookedText3DAsset& asset, std::string* error = nullptr) noexcept;

[[nodiscard]] bool write_dtext(
    const std::filesystem::path& path,
    const CookedText3DAsset& asset,
    std::string* error = nullptr);
[[nodiscard]] Text3DReadResult read_dtext(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max());

// CPU oracle for the Slug face coverage plus extruded side mesh. This exists for deterministic
// regression evidence; production rendering uses the checked-in HLSL shaders.
[[nodiscard]] bool write_text3d_preview_ppm(
    const std::filesystem::path& path,
    const CookedText3DAsset& asset,
    const Text3DPreviewOptions& options = {},
    std::string* error = nullptr);

} // namespace dve
