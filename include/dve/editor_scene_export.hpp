#pragma once

// Offline editor-scene export (packaging Phase 4, decision D2): turns an editable
// DVE_EDITOR_SCENE document (`.dvescene` + `.objects.rN/`) into the shipped runtime format,
// a DVOXSCENE v1 JSON manifest plus one `.dvox` per object. dve_player loads the result
// with load_scene_into_game_world(), so a shipped game never links dve_editor.
//
// Mapping (mirrors EditorPlaySession::start where DVOXSCENE v1 can express it):
//   voxel object            -> one .dvox (id = editor object id) + manifest entry
//   world transform         -> worldMatrix (document.world_transform, rigid, column-major)
//   flags.anchored          -> anchored (static collision); otherwise a dynamic body
//   flags.structural        -> structural
//   flags.collisionEnabled  -> generateCollision (false => visual-only in the player)
//   parent (attachment)     -> parent (manifest index; the player keeps it as hierarchy
//                              metadata, it does not attach bodies yet)
//   materials               -> the .dvox material table, taken from the editor material
//                              library (defaults, or a .dvematerials file) so colours and
//                              densities match the editor; unknown ids get a neutral material.
// Not expressible in DVOXSCENE v1, so reported as warnings (errors with `strict`):
//   hidden objects (skipped), objects without voxels (skipped), text3d / gabor-volume /
//   polygon (.dmesh) objects (skipped), components, tags, groups and layers (dropped),
//   prefab links (flattened: the instance's current voxels are exported).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"

namespace dve::editor {

struct EditorSceneExportOptions {
    // Folder (relative to the manifest's folder) that receives the .dvox files. Empty means
    // "<manifest stem>.objects", e.g. scenes/level.dvoxscene.json -> scenes/level.objects/.
    std::string objectDirectory;
    // Scene name written to the manifest; empty means the document name.
    std::string sceneName;
    // Treat every warning (something the runtime format cannot represent) as an error.
    bool strict{};
};

struct EditorSceneExportObject {
    EditorObjectId id{};
    std::string name;
    std::string file;               // relative to the manifest folder, forward slashes
    std::size_t index{};
    std::uint64_t voxelCount{};
    bool anchored{};
    bool collision{true};
    bool hasParent{};
};

struct EditorSceneExportResult {
    bool success{};
    std::string error;
    std::vector<std::string> warnings;
    std::filesystem::path manifestPath;
    std::vector<EditorSceneExportObject> objects;
    std::size_t skippedObjects{};
};

// `manifestPath` must end in ".dvoxscene.json". Existing .dvox files in the object folder
// that are not part of this export are removed, so re-exporting is idempotent. The manifest
// is written last (via a temporary file), after every .dvox succeeded.
[[nodiscard]] EditorSceneExportResult export_editor_scene(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const std::filesystem::path& manifestPath,
    const EditorSceneExportOptions& options = {});

} // namespace dve::editor
