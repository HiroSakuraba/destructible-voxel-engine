#pragma once
#include "dve/polygon_asset.hpp"
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
    BaseColor=1, Normal=2, MetallicRoughness=4, Emission=8, Opacity=16
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
    RigidTransform pose{}; // Frozen object/world mapping state, in source coordinate units.
};
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
};
struct MaterialTextureBakeResult {
    bool success{},cancelled{};
    std::string error;
    MaterialTextureBakeSettings settings;
    std::uint64_t sourceHash{},recipeKey{};
    std::uint64_t coveredPixels{},dilatedPixels{},surfaceSamples{};
    std::uint32_t chartCount{},safeMipLevels{};
    std::map<MaterialBakeMap,PolygonImage> images;
};
[[nodiscard]] const char* material_bake_map_name(MaterialBakeMap) noexcept;
[[nodiscard]] MaterialBakeValidation validate_material_bake(const CookedPolygonAsset&,
    const MaterialTextureBakeSettings&,const MaterialBakeControl& = {});
[[nodiscard]] MaterialTextureBakeResult bake_material_textures(const CookedPolygonAsset&,
    const MaterialTextureBakeSettings&,const MaterialBakeControl& = {});
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
    const MaterialBakeControl& = {},std::string* error=nullptr);
}
