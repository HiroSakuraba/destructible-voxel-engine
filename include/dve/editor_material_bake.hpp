#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include "dve/editor_modeling_topology.hpp"
#include "dve/render/material_texture_bake.hpp"

namespace dve::editor {
struct EditorObject;
// Editor-owned snapshots. Runtime export uses only the currently assigned polygon.
struct MaterialBakeRecipe {
    bool showingSource{};
    CookedPolygonAsset source,baked;
    std::optional<ModelingTopology> sourceTopology,bakedTopology;
    render::MaterialTextureBakeSettings settings,bakedSettings;
    std::uint64_t bakedSourceHash{},bakedAssetHash{};
    std::filesystem::path outputDirectory;
};
struct MaterialBakeStatus {bool stale{};std::string reason;std::uint64_t key{};};
[[nodiscard]] MaterialBakeStatus material_bake_status(const EditorObject&,const RigidTransform& worldPose);
[[nodiscard]] std::uint64_t material_bake_recipe_hash(const MaterialBakeRecipe&);
// Rebinds both snapshots (and their baseline hashes) to a new owning object id. Polygon content
// hashes include the object id, so duplicates, pastes and prefab instances must call this.
void remap_material_bake_ownership(EditorObject&,std::uint64_t objectId);
[[nodiscard]] bool validate_material_bake_recipe(const MaterialBakeRecipe&,std::string* error=nullptr);
[[nodiscard]] bool write_material_bake_recipe(const std::filesystem::path&,const MaterialBakeRecipe&,std::string* error=nullptr);
[[nodiscard]] std::optional<MaterialBakeRecipe> read_material_bake_recipe(const std::filesystem::path&,std::string* error=nullptr);
}
