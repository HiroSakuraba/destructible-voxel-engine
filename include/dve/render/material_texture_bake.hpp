#pragma once
#include "dve/polygon_asset.hpp"
#include "dve/render_environment.hpp"
#include <span>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dve::render {
enum class MaterialBakeMap : std::uint32_t {
    BaseColor=1, Normal=2, MetallicRoughness=4, Emission=8, Opacity=16, AmbientOcclusion=32, DiffuseLighting=64
};
inline constexpr std::uint32_t kMaterialBakeDefaultMaps=15;
struct MaterialTextureBakeSettings {
    std::uint32_t materialIndex{};
    std::uint32_t targetUv{};
    std::uint32_t resolution{512};
    std::uint32_t samplesPerAxis{2};
    std::uint32_t paddingPixels{8};
    std::uint32_t maps{kMaterialBakeDefaultMaps};
    std::uint64_t memoryBudgetBytes{512ULL*1024*1024};
    float metersPerUnit{1};
    // Empty selection retains the legacy single-slot contract. Sorted unique slot IDs otherwise.
    std::vector<std::uint32_t> materialSlots;
    bool allAssignedSlots{};
    bool generateAtlas{}; // independent triangle charts in UV1; source UV streams remain untouched
    Float3 scale{1,1,1};
    float texelsPerMeter{}; // 0 = explicit resolution, otherwise a minimum density (atlas only)
    std::uint32_t raySamples{16},seed{1};
    float aoDistanceMeters{1},rayBiasMeters{.001F},lightingRange{16};
    RenderEnvironment lighting; // frozen light rig; only lighting fields are serialized
    RigidTransform pose{}; // Frozen object/world mapping state, in source coordinate units.
};
struct MaterialBakeSceneMesh { CookedPolygonAsset asset; RigidTransform pose{}; };
// Snapshots exclude the bake target, which is always traced from its editable source.
using MaterialBakeScene = std::vector<MaterialBakeSceneMesh>;
[[nodiscard]] CookedPolygonAsset material_bake_scaled_mesh(const CookedPolygonAsset&,Float3 scale);
[[nodiscard]] std::uint64_t material_bake_scene_hash(std::span<const MaterialBakeSceneMesh>);
[[nodiscard]] std::string encode_material_bake_settings(const MaterialTextureBakeSettings&);
[[nodiscard]] bool decode_material_bake_settings(std::string_view,MaterialTextureBakeSettings&);
[[nodiscard]] bool valid_material_bake_settings(const MaterialTextureBakeSettings&);
[[nodiscard]] bool material_bake_selects(const MaterialTextureBakeSettings&,std::uint32_t slot);
struct MaterialBakeControl {
    const std::atomic<bool>* cancel{};
    std::function<void(float,std::string_view)> progress;
    [[nodiscard]] bool cancelled() const noexcept {return cancel&&cancel->load();}
};
struct MaterialBakeValidation {
    bool valid{};
    std::string error;
    std::uint32_t triangle{},otherTriangle{};
    std::uint64_t estimatedBytes{};
    std::vector<std::uint32_t> firstIndices,charts;
    std::vector<Float2> targetUvs; // per index-buffer corner, populated for generated UVs
    float utilization{},minimumTexelsPerMeter{};
    std::uint32_t resolution{};
    std::vector<std::string> warnings;
};
struct MaterialTextureBakeResult {
    bool success{},cancelled{};
    std::string error;
    MaterialTextureBakeSettings settings;
    std::uint64_t sourceHash{},recipeKey{};
    std::uint64_t coveredPixels{},dilatedPixels{},surfaceSamples{};
    std::uint32_t chartCount{},safeMipLevels{};
    std::uint64_t sceneHash{};
    std::vector<Float2> targetUvs;
    float utilization{},minimumTexelsPerMeter{};
    std::map<MaterialBakeMap,PolygonImage> images;
};
[[nodiscard]] const char* material_bake_map_name(MaterialBakeMap) noexcept;
// Deterministic projected triangle charts with resolution-aware gutters; never edits source data.
[[nodiscard]] MaterialBakeValidation prepare_material_bake_atlas(const CookedPolygonAsset&,
    const MaterialTextureBakeSettings&,const MaterialBakeControl& = {});
[[nodiscard]] MaterialBakeValidation validate_material_bake(const CookedPolygonAsset&,
    const MaterialTextureBakeSettings&,const MaterialBakeControl& = {});
[[nodiscard]] MaterialTextureBakeResult bake_material_textures(const CookedPolygonAsset&,
    const MaterialTextureBakeSettings&,const MaterialBakeControl& = {},
    std::span<const MaterialBakeSceneMesh> scene = {});
// Appends a derived material and remaps target triangles. Source slots/maps stay intact.
// Target vertices are copied with neutral colors because color/alpha were baked already.
[[nodiscard]] std::optional<CookedPolygonAsset> make_baked_material_asset(
    const CookedPolygonAsset&,const MaterialTextureBakeResult&,std::string* error=nullptr);
[[nodiscard]] std::string material_bake_manifest(const MaterialTextureBakeResult&);
// Creates a new bundle directory by staging and atomic rename. Never overwrites a bundle.
// Includes source.dmesh, PNG maps and provenance.json for explicit rebaking.
// Includes baked.dmesh only when all four required material maps were selected.
[[nodiscard]] bool write_material_bake_bundle(const std::filesystem::path& newDirectory,
    const CookedPolygonAsset& source,const MaterialTextureBakeResult&,
    const MaterialBakeControl& = {},std::string* error=nullptr,
    std::span<const MaterialBakeSceneMesh> scene = {});
}
