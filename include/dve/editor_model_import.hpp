#pragma once

// File > Import Model options and File > Reimport Model (follow-up to #101).
//
// The options panel reads the model once, lists its mesh nodes, and estimates how many voxels
// the import can make for the chosen voxel size and scale. The estimate is the sum of the
// included nodes' bounding boxes in voxel cells: an upper bound on what the voxelizer can fill,
// and the number the hard import cap is checked against before any voxelizing starts.
// The settings are stored on the import group (a "dve.editor_import" component), so Reimport
// rebuilds the parts from the file with the same choices.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/component.hpp"
#include "dve/editor_document.hpp"

namespace dve::editor {

inline constexpr std::string_view kImportSettingsComponent = "dve.editor_import";
// Hard cap on the estimated voxel cells of one import. Above it the import is refused with the
// estimate and what to change; the voxelizer's own working budget is set to the same number.
inline constexpr std::uint64_t kMaxImportVoxels = 32'000'000ULL;
inline constexpr float kMinImportVoxelSize = 0.005F;
inline constexpr float kMaxImportVoxelSize = 10.0F;
inline constexpr float kMinImportScale = 0.001F;
inline constexpr float kMaxImportScale = 1000.0F;

struct EditorImportSettings {
    float voxelSizeMeters{0.10F};
    float scale{1.0F};
    VoxelizationMode mode{VoxelizationMode::Solid};
    // Mesh nodes left out, by node name (the cooker's per-node rules are keyed by name, so nodes
    // that share a name are included or left out together).
    std::set<std::string> excludedNodes;
};

struct EditorImportNode {
    std::string name;
    std::string path;          // parent/child path, for display
    std::uint64_t triangles{};
};

struct EditorImportEstimate {
    std::uint64_t voxels{};        // upper bound: included nodes' bounding boxes, in voxel cells
    std::size_t includedNodes{};
    std::size_t totalNodes{};
    std::uint64_t triangles{};     // of the included nodes
    std::string refusal;           // non-empty: the import cannot run with these settings
};

[[nodiscard]] std::string_view import_mode_label(VoxelizationMode mode) noexcept;
[[nodiscard]] std::vector<EditorImportNode> list_import_nodes(const ImportedScene& scene);
// Empty when the settings are usable; otherwise why not.
[[nodiscard]] std::string validate_import_settings(const EditorImportSettings& settings);
[[nodiscard]] EditorImportEstimate estimate_import(const ImportedScene& scene, const EditorImportSettings& settings);
// A copy of the scene scaled uniformly about the origin (mesh positions and node translations).
[[nodiscard]] ImportedScene scaled_import_scene(const ImportedScene& scene, float scale);
// The cooker settings for these choices (voxel size, mode, left-out nodes, the cap as budget).
[[nodiscard]] SceneCookSettings import_cook_settings(const ImportedScene& scene, const EditorImportSettings& settings);

[[nodiscard]] Component make_import_settings_component(const EditorImportSettings& settings, ComponentId id);
[[nodiscard]] std::optional<EditorImportSettings> read_import_settings(const EditorObject& object);
[[nodiscard]] bool is_model_source(const std::filesystem::path& path);

} // namespace dve::editor
