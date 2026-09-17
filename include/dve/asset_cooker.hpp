#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/types.hpp"
#include "dve/voxel_object.hpp"

namespace dve {

struct Float2 {
    float x{};
    float y{};
};

struct Float4 {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

struct VoxelMaterialLayer {
    MaterialId sourceMaterial{kAirMaterial};
    float weight{0.0F};
    MaterialLayerBlendMode blendMode{MaterialLayerBlendMode::Lerp};
    bool enabled{true};
};

inline constexpr std::size_t kMaximumVoxelMaterialLayers = 4;

// Column-major 4x4 matrix, matching glTF's matrix representation.
struct Matrix4 {
    std::array<float, 16> values{};

    [[nodiscard]] static Matrix4 identity() noexcept;
};

[[nodiscard]] Matrix4 multiply(const Matrix4& a, const Matrix4& b) noexcept;
[[nodiscard]] Float3 transform_point(const Matrix4& matrix, Float3 point) noexcept;
[[nodiscard]] Float3 transform_vector(const Matrix4& matrix, Float3 vector) noexcept;

struct ImportedVertex {
    Float3 position{};
    Float3 normal{};
    Float2 texcoord{};
    Float4 color{1.0F, 1.0F, 1.0F, 1.0F};
    Float2 texcoord1{};
};

struct ImportedTriangle {
    std::array<std::uint32_t, 3> indices{};
    std::uint32_t materialIndex{};
    std::uint32_t sourcePrimitive{};
};

enum class ImportedAlphaMode : std::uint8_t {
    Opaque,
    Mask,
    Blend,
};

enum class ImportedWrapMode : std::uint16_t {
    ClampToEdge = 33071,
    MirroredRepeat = 33648,
    Repeat = 10497,
};

enum class ImportedTextureFilter : std::uint16_t {
    Nearest = 9728,
    Linear = 9729,
};

struct ImportedImage {
    std::string name;
    std::string mimeType;
    std::uint32_t width{};
    std::uint32_t height{};
    // Canonical top-left-origin RGBA8 pixels used directly by glTF UV sampling.
    std::vector<std::uint8_t> rgba8;
};

struct ImportedSampler {
    ImportedWrapMode wrapS{ImportedWrapMode::Repeat};
    ImportedWrapMode wrapT{ImportedWrapMode::Repeat};
    ImportedTextureFilter minFilter{ImportedTextureFilter::Linear};
    ImportedTextureFilter magFilter{ImportedTextureFilter::Linear};
};

struct ImportedTexture {
    std::string name;
    std::uint32_t imageIndex{};
    std::optional<std::uint32_t> samplerIndex;
};

struct ImportedMaterial {
    std::string name;
    Float4 baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    Float3 emissiveFactor{};
    float metallicFactor{1.0F};
    float roughnessFactor{1.0F};
    ImportedAlphaMode alphaMode{ImportedAlphaMode::Opaque};
    float alphaCutoff{0.5F};
    bool doubleSided{};
    std::optional<std::uint32_t> baseColorTexture;
    std::uint32_t baseColorTexcoord{};
    std::optional<std::uint32_t> metallicRoughnessTexture;
    std::uint32_t metallicRoughnessTexcoord{};
    std::optional<std::uint32_t> normalTexture;
    std::uint32_t normalTexcoord{};
    float normalScale{1.0F};
    std::optional<std::uint32_t> emissiveTexture;
    std::uint32_t emissiveTexcoord{};
    // glTF normally carries opacity in base-color alpha. This optional channel is retained for
    // imported formats and DVE-authored materials that provide a dedicated mask.
    std::optional<std::uint32_t> opacityTexture;
    std::uint32_t opacityTexcoord{};
};

struct ImportedMesh {
    std::string name;
    std::vector<ImportedVertex> vertices;
    std::vector<ImportedTriangle> triangles;
};

struct ImportedNode {
    std::string name;
    Matrix4 localTransform{Matrix4::identity()};
    Matrix4 worldTransform{Matrix4::identity()};
    std::optional<std::uint32_t> parent;
    std::vector<std::uint32_t> children;
    std::optional<std::uint32_t> mesh;
    // Canonical JSON for the node's extras value. Empty when extras are absent.
    std::string extrasJson;
};

struct ImportedScene {
    std::string name;
    std::vector<ImportedImage> images;
    std::vector<ImportedSampler> samplers;
    std::vector<ImportedTexture> textures;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedNode> nodes;
    std::vector<std::uint32_t> roots;
};

enum class VoxelizationMode : std::uint8_t {
    Solid,
    Shell,
    SurfaceOnly,
};

struct VoxelMaterialDefinition {
    std::string name;
    // Linear RGBA. Base-color image texels are decoded from sRGB before multiplication.
    Float4 baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Float3 emissive{};
    float metallic{};
    float roughness{1.0F};
    // Dielectric F0 control; 0.5 corresponds to roughly four-percent reflectance.
    float specular{0.5F};
    MaterialShadingModel shadingModel{MaterialShadingModel::StandardPBR};
    MaterialBlendMode blendMode{MaterialBlendMode::Opaque};
    float subsurfaceScatterDistanceMeters{0.0F};
    Float3 subsurfaceColor{1.0F, 1.0F, 1.0F};
    // Clear-coat is a second dielectric lobe layered above the base BRDF.
    float clearCoat{0.0F};
    float clearCoatRoughness{0.1F};
    // Two-sided foliage uses a view-facing normal for the front lobe and a back-light
    // transmission term colored independently from the ordinary base color.
    Float3 foliageColor{0.5F, 0.8F, 0.35F};
    float foliageTransmittance{0.5F};
    float foliageWrap{0.35F};
    // Up to four uniform-weight material layers. Runtime/editor code resolves these into an
    // effective material record before upload; the stack remains here for persistence and
    // live weight editing.
    std::vector<VoxelMaterialLayer> layers;
    float densityKilogramsPerCubicMeter{1000.0F};
    float structuralStrength{1.0F};
    float fractureResistance{1.0F};
    float flammability{};
    float thermalConductivity{};
    bool transparent{};
    bool structural{true};
};

struct ModelImportOptions {
    bool strict{true};
    bool preserveNodeHierarchy{true};
    std::uint32_t maximumMaterials{255};
    std::uint32_t maximumImageDimension{16384};
    std::uint64_t maximumDecodedImageBytes{512ULL * 1024ULL * 1024ULL};
};

struct VoxelizeSettings {
    VoxelizationMode mode{VoxelizationMode::Solid};
    float voxelSizeMeters{0.10F};
    std::uint32_t shellThicknessVoxels{1};
    std::uint32_t supersampleFactor{1};
    float downsampleCoverageThreshold{0.125F};
    MaterialId interiorMaterial{kAirMaterial};
    std::uint64_t objectId{1};
    std::uint64_t maximumWorkingVoxels{128ULL * 1024ULL * 1024ULL};
    bool preserveThinSurface{true};
    // Includes air. Valid range is [2, 256]. The cooker deterministically reduces
    // texture-derived color variants to this limit without merging physical source materials.
    std::uint32_t maximumPaletteMaterials{256};
};

struct ImportDiagnostic {
    enum class Severity : std::uint8_t { Info, Warning, Error };
    Severity severity{Severity::Info};
    std::string code;
    std::string message;
};

struct AssetCookerStats {
    std::uint64_t sourceVertices{};
    std::uint64_t sourceTriangles{};
    std::uint64_t degenerateTriangles{};
    std::uint64_t unsupportedPrimitives{};
    std::uint64_t boundaryEdges{};
    std::uint64_t nonManifoldEdges{};
    std::uint64_t meshIslands{};
    std::uint64_t decodedImages{};
    std::uint64_t decodedImageBytes{};
    std::uint64_t textureSamples{};
    std::uint64_t alphaRejectedSamples{};
    std::uint64_t uniqueMaterialSamples{};
    std::uint64_t paletteMaterials{};
    std::uint64_t surfaceVoxels{};
    std::uint64_t interiorVoxels{};
    std::uint64_t outputVoxels{};
    std::uint64_t outputBricks{};
    Int3 minimumVoxel{};
    Int3 maximumVoxel{};
};

struct ImportedModelResult {
    ImportedScene scene;
    std::vector<ImportDiagnostic> diagnostics;
    bool success{};
};

struct CookedVoxelAsset {
    float voxelSizeMeters{0.10F};
    std::vector<VoxelMaterialDefinition> materials;
    VoxelObject object;
    AssetCookerStats stats;
    std::vector<ImportDiagnostic> diagnostics;

    explicit CookedVoxelAsset(std::uint64_t objectId = 1) : object(objectId) {}
};

struct NodeCookSettings {
    bool ignore{};
    std::optional<VoxelizationMode> mode;
    std::optional<float> voxelSizeMeters;
    std::optional<std::uint32_t> shellThicknessVoxels;
    std::optional<MaterialId> interiorMaterial;
    std::optional<std::uint64_t> objectId;
    std::optional<bool> anchored;
    std::optional<bool> structural;
    std::optional<bool> generateCollision;
};

struct SceneCookSettings {
    VoxelizeSettings defaults{};
    bool splitByNode{true};
    std::map<std::string, NodeCookSettings, std::less<>> nodeOverrides;
};

struct CookedVoxelObject {
    std::string name;
    std::string nodePath;
    std::uint32_t sourceNode{};
    std::optional<std::uint32_t> parentObject;
    Matrix4 worldTransform{Matrix4::identity()};
    bool anchored{};
    bool structural{true};
    bool generateCollision{true};
    CookedVoxelAsset asset;

    explicit CookedVoxelObject(std::uint64_t objectId = 1) : asset(objectId) {}
};

struct CookedVoxelScene {
    std::string name;
    std::vector<CookedVoxelObject> objects;
    std::vector<ImportDiagnostic> diagnostics;
    bool success{};
};

[[nodiscard]] ImportedModelResult import_model(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& options = {});

[[nodiscard]] CookedVoxelAsset voxelize_scene(
    const ImportedScene& scene,
    const VoxelizeSettings& settings = {});

[[nodiscard]] CookedVoxelAsset cook_model(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& importOptions = {},
    const VoxelizeSettings& voxelizeSettings = {});

[[nodiscard]] CookedVoxelScene voxelize_scene_objects(
    const ImportedScene& scene,
    const SceneCookSettings& settings = {});

[[nodiscard]] CookedVoxelScene cook_model_scene(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& importOptions = {},
    const SceneCookSettings& settings = {});

// Applies global offline-cooker defaults from a JSON sidecar. Command-line or editor
// overrides can be layered afterward.
[[nodiscard]] bool apply_voxelize_settings_json(
    const std::filesystem::path& path,
    VoxelizeSettings& settings,
    std::string* error = nullptr);

// Applies defaults plus sidecar node rules. Node rules are keyed by exact imported node name.
[[nodiscard]] bool apply_scene_cook_settings_json(
    const std::filesystem::path& path,
    SceneCookSettings& settings,
    std::string* error = nullptr);

[[nodiscard]] bool write_import_report_html(
    const std::filesystem::path& path,
    std::string_view sourceName,
    const CookedVoxelAsset& asset,
    std::string* error = nullptr);

// Writes one .dvox file per cooked object plus a deterministic JSON scene manifest.
// manifestPath is normally <asset>.dvoxscene.json.
[[nodiscard]] bool write_dvox_scene_package(
    const std::filesystem::path& manifestPath,
    const CookedVoxelScene& scene,
    std::string* error = nullptr);

[[nodiscard]] const char* to_string(VoxelizationMode mode) noexcept;
[[nodiscard]] const char* to_string(ImportDiagnostic::Severity severity) noexcept;

} // namespace dve
