#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/navigation_mesh.hpp"
#include "dve/polygon_asset.hpp"

namespace dve {

enum class HeightmapFormat : std::uint8_t { Automatic, Png, Pgm, Raw16LittleEndian, Raw16BigEndian };

struct HeightmapImportSettings {
    HeightmapFormat format{HeightmapFormat::Automatic};
    std::uint32_t rawWidth{};
    std::uint32_t rawHeight{};
    bool flipVertical{};
    bool invert{};
    std::uint32_t maximumDimension{16384U};
    std::uint64_t maximumSamples{268'435'456ULL};
};

struct HeightmapImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<float> samples; // normalized [0,1], row-major, top-left origin
    std::uint16_t sourceBitDepth{};
    std::string sourceFormat;

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
    [[nodiscard]] float at(std::uint32_t x, std::uint32_t y) const noexcept;
    [[nodiscard]] float sample_bilinear(float x, float y) const noexcept;
};

struct HeightmapTerrainSettings {
    Float3 originMeters{};
    float spacingXMeters{1.0F};
    float spacingZMeters{1.0F};
    float heightScaleMeters{100.0F};
    float heightOffsetMeters{};
    float solidBaseHeightMeters{-10.0F};
    float polygonSkirtDepthMeters{};
    bool alternateTriangleDiagonals{true};
    bool generatePolygonAsset{true};
    bool generateVoxelAsset{};
    bool generateNavigationMesh{true};
    std::uint64_t objectId{1U};
    std::string materialName{"Terrain"};
    Float4 baseColor{0.28F,0.42F,0.18F,1.0F};
    float roughness{0.9F};
    float voxelSizeMeters{0.25F};
    std::uint64_t maximumWorkingVoxels{256ULL*1024ULL*1024ULL};
    std::uint64_t maximumTerrainVertices{1'048'576ULL};
    std::uint64_t maximumTerrainTriangles{2'097'152ULL};
    NavigationBuildSettings navigation{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

struct HeightmapTerrainDiagnostic {
    enum class Severity : std::uint8_t { Information, Warning, Error };
    Severity severity{Severity::Information};
    std::string code;
    std::string message;
};

struct HeightmapTerrainBuildResult {
    HeightmapImage heightmap{};
    std::optional<CookedPolygonAsset> polygon;
    std::optional<CookedVoxelAsset> voxel;
    std::optional<NavigationMesh> navigation;
    std::vector<HeightmapTerrainDiagnostic> diagnostics;

    [[nodiscard]] bool success() const noexcept;
};

[[nodiscard]] std::optional<HeightmapImage> import_heightmap(
    const std::filesystem::path& path,
    const HeightmapImportSettings& settings = {},
    std::string* error = nullptr);

[[nodiscard]] ImportedScene build_heightmap_imported_scene(
    const HeightmapImage& image,
    const HeightmapTerrainSettings& settings,
    bool closeSolidVolume = false);

[[nodiscard]] std::vector<NavigationTriangle> heightmap_navigation_triangles(
    const HeightmapImage& image,
    const HeightmapTerrainSettings& settings);

[[nodiscard]] HeightmapTerrainBuildResult cook_heightmap_terrain(
    const std::filesystem::path& path,
    const HeightmapImportSettings& importSettings = {},
    const HeightmapTerrainSettings& terrainSettings = {});

} // namespace dve
