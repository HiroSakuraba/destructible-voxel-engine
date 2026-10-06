#pragma once

// Offline editor-scene export (packaging Phase 4, decision D2): turns an editable
// DVE_EDITOR_SCENE document (`.dvescene` + `.objects.rN/`) into the shipped runtime format,
// a DVOXSCENE v1 JSON manifest plus one asset file per object. dve_player loads the result
// with load_scene_into_game_world(), so a shipped game never links dve_editor.
//
// Mapping (mirrors EditorPlaySession::start):
//   voxel object            -> one .dvox (id = editor object id) + manifest entry
//   polygon (.dmesh) object -> the validated .dmesh copied next to the .dvox files, manifest
//                              extension geometry "polygon" (GameWorld::spawn_cooked_polygon_asset;
//                              generateCollision=false => visual-only polygon)
//   3D text (.dtext)        -> baked into voxels at the object's voxel size (see
//                              bake_text3d_voxels): face colour on the front/back layers, side
//                              colour inside. The runtime has no Slug text path.
//   Gabor volume            -> baked into visual-only voxels (see bake_gabor_volume_voxels): the
//                              runtime has no volume renderer, so the density field is
//                              thresholded to an opaque voxel shell with the volume's tint.
//   world transform         -> worldMatrix (document.world_transform, rigid, column-major)
//   flags.anchored          -> anchored (static collision); otherwise a dynamic body
//   flags.structural        -> structural
//   flags.collisionEnabled  -> generateCollision (false => visual-only in the player)
//   parent                  -> parent (manifest index)
//   attachment              -> extension "attachment" (socket, inherit flags): a real GameWorld
//                              attachment in the player, so the child follows its parent
//   components              -> extension "components" (ids, enabled, typed properties); tags,
//                              groups and layer travel as a dve.membership component
//   materials               -> the .dvox material table, taken from the editor material
//                              library (defaults, or a .dvematerials file) so colours and
//                              densities match the editor; unknown ids get a neutral material.
// Not expressible, so reported as warnings (errors with `strict`): hidden objects (skipped),
// objects without voxels (skipped), .dmesh objects whose file is missing or invalid (skipped),
// bakes that produce nothing or exceed the budget (skipped), components the runtime cannot
// take (unknown dve.* types, invalid values; dropped; dve.prefab_instance is editor-only and
// dropped silently because prefab links are flattened), prefab links (flattened: the
// instance's current voxels are exported). Bakes are reported in `notes`, not as warnings.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"

namespace dve::editor {

struct EditorSceneExportOptions {
    // Folder (relative to the manifest's folder) that receives the object files. Empty means
    // "<manifest stem>.objects", e.g. scenes/level.dvoxscene.json -> scenes/level.objects/.
    std::string objectDirectory;
    // Scene name written to the manifest; empty means the document name.
    std::string sceneName;
    // Root for relative source assets (.dmesh). Empty: the nearest folder above the document
    // that holds project.dveproject, else the document's own folder, else the current folder.
    std::filesystem::path projectRoot;
    // Treat every warning (something the runtime format cannot represent) as an error.
    bool strict{};
    // A Gabor voxel is solid when a slab one voxel thick would be at least this opaque:
    // 1 - exp(-density * voxelSize) >= threshold. Must be in (0, 1).
    float gaborOpacityThreshold{0.5F};
    // Upper bound on the cells a single text/Gabor bake may visit (its voxel bounding box).
    std::uint64_t maximumBakeCells{8ULL * 1024ULL * 1024ULL};
    // Upper bound on density evaluations (cells x primitives) for one Gabor bake.
    std::uint64_t maximumGaborEvaluations{2ULL * 1024ULL * 1024ULL * 1024ULL};
};

enum class EditorSceneExportKind : std::uint8_t { Voxel, Polygon, BakedText3D, BakedGaborVolume };

struct EditorSceneExportObject {
    EditorObjectId id{};
    std::string name;
    std::string file;               // relative to the manifest folder, forward slashes
    std::size_t index{};
    std::uint64_t voxelCount{};     // 0 for polygon objects
    bool anchored{};
    bool collision{true};
    bool hasParent{};
    bool attached{};                // written with an attachment extension
    EditorSceneExportKind kind{EditorSceneExportKind::Voxel};
    std::size_t componentCount{};   // components written (including a synthesized dve.membership)
};

struct EditorSceneExportResult {
    bool success{};
    std::string error;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;  // informational (bakes); never an error, even with strict
    std::filesystem::path manifestPath;
    std::vector<EditorSceneExportObject> objects;
    std::size_t skippedObjects{};
};

// `manifestPath` must end in ".dvoxscene.json". Existing .dvox/.dmesh files in the object
// folder that are not part of this export are removed, so re-exporting is idempotent. The
// manifest is written last (via a temporary file), after every object file succeeded.
[[nodiscard]] EditorSceneExportResult export_editor_scene(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const std::filesystem::path& manifestPath,
    const EditorSceneExportOptions& options = {});

// Voxelizes cooked 3D text in its own local frame (x right, y up, extruded along z around 0)
// at `voxelSizeMeters`: a voxel column is filled when its centre is inside the glyph outline
// (the side mesh's contours, with the style's fill rule); the front and back layers use
// material 1 (face colour), inner layers material 2 (side colour). Physical properties come
// from the library entries for style.faceMaterialId / sideMaterialId when they exist.
[[nodiscard]] std::optional<CookedVoxelAsset> bake_text3d_voxels(
    const CookedText3DAsset& text, float voxelSizeMeters, const EditorMaterialLibrary& materials,
    std::uint64_t objectId, std::uint64_t maximumCells, std::string* error = nullptr);

// Voxelizes a Gabor volume's density field (evaluate_gabor_density at voxel centres, in the
// asset's local frame) with the opacity threshold above; one material with the volume's
// albedo tint (plus emission) as base colour.
[[nodiscard]] std::optional<CookedVoxelAsset> bake_gabor_volume_voxels(
    const GaborVolumeAsset& volume, float voxelSizeMeters, float opacityThreshold,
    std::uint64_t objectId, std::uint64_t maximumCells, std::uint64_t maximumEvaluations,
    std::string* error = nullptr);

} // namespace dve::editor
