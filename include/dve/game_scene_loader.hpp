#pragma once

// Runtime (non-editor) scene loading: DVOXSCENE JSON + .dvox from a ContentSource into a
// GameWorld. This is what a shipped game / dve_player uses instead of EditorPlaySession,
// which builds its GameWorld from an in-memory EditorDocument and lives in dve_editor.
//
// Per object (in manifest index order, so GameObjectIds are deterministic for a fresh world):
//   - the .dvox is read through the ContentSource (relative to the manifest's folder) with
//     RuntimeSceneLoadOptions::maximumDvoxBytesPerObject, decoded in memory, and checked with
//     validate_dvoxscene_asset() exactly like RuntimeSceneWorld;
//   - anchored => static collision, otherwise a dynamic body; `structural` is passed through;
//     the asset's own per-material densities drive mass (GameWorld::spawn_cooked_asset);
//   - generateCollision=false => a marker object (name + transform, no voxels/body), the same
//     choice EditorPlaySession makes for objects with collision disabled. GameWorld has no
//     "visual-only voxels" object yet; see the Phase 2 notes in the PR.
//   - `parent` is scene hierarchy metadata (as in RuntimeSceneWorld) and is reported in the
//     result; with attachChildrenToParents it becomes a GameWorld attachment (child forced
//     dynamic, world transform preserved), mirroring EditorPlaySession's attachment pass.
// Loading is all-or-nothing: every asset is read and validated before the first object is
// created, and objects created before a later spawn/attach failure are destroyed again.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/content_source.hpp"
#include "dve/game_world.hpp"
#include "dve/runtime_scene.hpp"

namespace dve {

struct GameSceneLoadOptions {
    // Limits and policy shared with RuntimeSceneWorld (maximumManifestBytes, maximumObjects,
    // maximumDvoxBytesPerObject, expectedVoxelSizeMeters/voxelSizeTolerance). Streaming and
    // body-creation flags in this struct are not used by the GameWorld loader.
    RuntimeSceneLoadOptions limits{};
    bool attachChildrenToParents{false};
};

struct GameSceneLoadedObject {
    std::size_t index{};
    std::uint64_t sceneObjectId{};
    std::string name;
    std::string assetPath;                         // content path of the .dvox
    GameObjectId gameObjectId{kInvalidGameObjectId};
    std::optional<GameObjectId> parentGameObjectId;
    bool anchored{};
    bool collision{true};                          // false => spawned as a marker
    bool attached{};
};

struct GameSceneLoadResult {
    RuntimeSceneError error{};
    // Set when the failure came from the ContentSource (e.g. IntegrityFailure for a
    // corrupted pak entry, NotFound, LimitExceeded) so callers can react to the exact cause.
    ContentError contentError{};
    std::string sceneName;
    std::string manifestPath;
    std::vector<GameSceneLoadedObject> objects;

    [[nodiscard]] explicit operator bool() const noexcept { return !error; }
};

[[nodiscard]] GameSceneLoadResult load_scene_into_game_world(
    const ContentSource& content,
    std::string_view sceneManifestPath,
    GameWorld& world,
    const GameSceneLoadOptions& options = {});

} // namespace dve
