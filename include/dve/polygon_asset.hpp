#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/material_mapping.hpp"
#include "dve/material_layers.hpp"

namespace dve {

struct PolygonVertex {
    Float3 position{};
    Float3 normal{};
    Float4 tangent{1.0F, 0.0F, 0.0F, 1.0F};
    Float2 texcoord{};
    Float4 color{1.0F, 1.0F, 1.0F, 1.0F};
    Float2 texcoord1{};
};

struct PolygonBounds {
    Float3 minimum{};
    Float3 maximum{};
};

enum class PolygonTextureColorSpace : std::uint8_t { Linear, Srgb };

struct PolygonTextureBinding {
    std::optional<std::uint32_t> texture;
    std::uint32_t texcoord{};
    PolygonTextureColorSpace colorSpace{PolygonTextureColorSpace::Linear};
};

// Per-pixel controls for one VoxelMaterialLayer entry. The source material and uniform authored
// weight remain in VoxelMaterialDefinition::layers; this binding adds mask/height data and the
// channel policy required by the CPU reference compositor. Missing bindings preserve the legacy
// uniform-layer behavior.
struct PolygonMaterialLayerBinding {
    MaterialLayerSemantic semantic{MaterialLayerSemantic::Custom};
    PolygonTextureBinding mask{};
    PolygonTextureBinding height{};
    float maskScale{1.0F};
    float maskBias{};
    float heightBlendStrength{1.0F};
    float heightBlendBias{};
    float heightBlendTransition{0.1F};
    MaterialLayerOpacityPolicy opacityPolicy{MaterialLayerOpacityPolicy::PreserveBase};
    bool enabled{true};
};

struct PolygonMaterialBinding {
    bool doubleSided{};
    float alphaCutoff{0.5F};
    PolygonTextureBinding baseColor{std::nullopt, 0U, PolygonTextureColorSpace::Srgb};
    PolygonTextureBinding metallicRoughness{};
    PolygonTextureBinding normal{};
    float normalScale{1.0F};
    PolygonTextureBinding emissive{std::nullopt, 0U, PolygonTextureColorSpace::Srgb};
    PolygonTextureBinding opacity{};

    // v1.63 mapping foundation. Height/parallax is visual-only and does not alter geometry,
    // collision, depth, or shadow-caster silhouettes.
    MaterialMappingSettings mapping{};
    PolygonTextureBinding height{};
    PolygonTextureBinding detailBaseColor{std::nullopt, 0U, PolygonTextureColorSpace::Srgb};
    PolygonTextureBinding detailNormal{};
    PolygonTextureBinding detailRoughness{};
    float detailNormalScale{1.0F};

    // Optional per-pixel bindings corresponding by index to VoxelMaterialDefinition::layers.
    // At most four entries are accepted. Fewer entries mean the remaining layers use their
    // legacy uniform authored weights.
    std::vector<PolygonMaterialLayerBinding> layers;

    // Compatibility accessors for v1.33 callers.
    [[nodiscard]] const std::optional<std::uint32_t>& base_color_texture() const noexcept {
        return baseColor.texture;
    }
};

struct PolygonSubmesh {
    std::string name;
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::uint32_t materialIndex{};
};

struct PolygonImage {
    std::string name;
    std::string mimeType;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba8;
};

struct PolygonSampler {
    ImportedWrapMode wrapS{ImportedWrapMode::Repeat};
    ImportedWrapMode wrapT{ImportedWrapMode::Repeat};
    ImportedTextureFilter minFilter{ImportedTextureFilter::Linear};
    ImportedTextureFilter magFilter{ImportedTextureFilter::Linear};
};

struct PolygonTexture {
    std::string name;
    std::uint32_t imageIndex{};
    std::optional<std::uint32_t> samplerIndex;
};

struct CookedPolygonAsset {
    std::uint64_t objectId{1};
    std::vector<VoxelMaterialDefinition> materials;
    std::vector<PolygonMaterialBinding> materialBindings;
    std::vector<PolygonImage> images;
    std::vector<PolygonSampler> samplers;
    std::vector<PolygonTexture> textures;
    std::vector<PolygonVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<PolygonSubmesh> submeshes;
    PolygonBounds bounds{};
    std::uint64_t contentHash{};
};

struct PolygonCookOptions {
    std::uint64_t objectId{1};
    bool generateMissingNormals{true};
    bool generateTangents{true};
    bool preserveTextures{true};
    std::uint32_t maximumMaterials{256};
    std::uint64_t maximumVertices{16ULL * 1024ULL * 1024ULL};
    std::uint64_t maximumIndices{48ULL * 1024ULL * 1024ULL};
};

enum class PolygonAssetErrorCode : std::uint8_t {
    NoError,
    Empty,
    LimitExceeded,
    InvalidVertex,
    InvalidIndex,
    InvalidSubmesh,
    InvalidMaterial,
    InvalidTexture,
    InvalidBounds,
    HashMismatch,
    Io,
    UnsupportedVersion,
    Corrupt,
};

struct PolygonAssetValidationResult {
    PolygonAssetErrorCode code{PolygonAssetErrorCode::NoError};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept {
        return code == PolygonAssetErrorCode::NoError;
    }
};

struct PolygonAssetReadResult {
    CookedPolygonAsset asset{};
    PolygonAssetErrorCode code{PolygonAssetErrorCode::NoError};
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept {
        return code == PolygonAssetErrorCode::NoError;
    }
};

[[nodiscard]] std::uint64_t polygon_asset_content_hash(const CookedPolygonAsset& asset) noexcept;
[[nodiscard]] PolygonAssetValidationResult validate_polygon_asset(const CookedPolygonAsset& asset) noexcept;
[[nodiscard]] CookedPolygonAsset cook_polygon_scene(
    const ImportedScene& scene,
    const PolygonCookOptions& options = {},
    std::vector<ImportDiagnostic>* diagnostics = nullptr);
[[nodiscard]] CookedPolygonAsset cook_polygon_model(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& importOptions = {},
    const PolygonCookOptions& polygonOptions = {},
    std::vector<ImportDiagnostic>* diagnostics = nullptr);

[[nodiscard]] bool write_dmesh(
    const std::filesystem::path& path,
    const CookedPolygonAsset& asset,
    std::string* error = nullptr);
[[nodiscard]] PolygonAssetReadResult read_dmesh(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max());

} // namespace dve
