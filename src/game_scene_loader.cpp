#include "dve/game_scene_loader.hpp"

#include <exception>
#include <utility>

#include "dve/dvox.hpp"

namespace dve {
namespace {

[[nodiscard]] RuntimeSceneErrorCode scene_code_for(ContentErrorCode code, bool manifest) noexcept {
    switch (code) {
    case ContentErrorCode::InvalidPath: return RuntimeSceneErrorCode::PathEscape;
    case ContentErrorCode::NotFound: return manifest ? RuntimeSceneErrorCode::Io : RuntimeSceneErrorCode::MissingAsset;
    case ContentErrorCode::LimitExceeded: return RuntimeSceneErrorCode::LimitExceeded;
    case ContentErrorCode::IntegrityFailure:
        return manifest ? RuntimeSceneErrorCode::Io : RuntimeSceneErrorCode::DvoxReadFailed;
    case ContentErrorCode::Io:
    case ContentErrorCode::NoError: break;
    }
    return RuntimeSceneErrorCode::Io;
}

struct StagedObject {
    RuntimeSceneObjectMetadata metadata;
    std::string assetPath;
    std::optional<CookedVoxelAsset> asset;
};

} // namespace

GameSceneLoadResult load_scene_into_game_world(
    const ContentSource& content,
    std::string_view sceneManifestPath,
    GameWorld& world,
    const GameSceneLoadOptions& options) {
    GameSceneLoadResult result;
    const auto failContent = [&](const ContentError& contentError, bool manifest, std::optional<std::uint64_t> objectId,
                                 std::string_view what) {
        result.contentError = contentError;
        result.error = {scene_code_for(contentError.code, manifest),
                        std::string(what) + " (" + content.describe() + "): " +
                            (contentError.message.empty() ? to_string(contentError.code) : contentError.message),
                        contentError.path, objectId};
        return result;
    };

    std::string pathError;
    const auto manifestPath = normalize_content_path(sceneManifestPath, &pathError);
    if (!manifestPath) {
        return failContent({ContentErrorCode::InvalidPath, pathError, std::string(sceneManifestPath)}, true,
                           std::nullopt, "invalid scene path");
    }
    result.manifestPath = *manifestPath;

    ContentError readError;
    const auto text = content.read_text(*manifestPath, &readError, options.limits.maximumManifestBytes);
    if (!text) return failContent(readError, true, std::nullopt, "could not read scene manifest");

    RuntimeSceneError parseError;
    auto manifest = parse_dvoxscene_manifest(*text, options.limits, &parseError);
    if (!manifest) {
        if (parseError.path.empty()) parseError.path = *manifestPath;
        result.error = std::move(parseError);
        return result;
    }
    result.sceneName = manifest->name;

    // Stage: read + decode + validate every asset before creating any object.
    std::vector<StagedObject> staged;
    staged.reserve(manifest->objects.size());
    for (RuntimeSceneObjectMetadata& metadata : manifest->objects) {
        StagedObject item;
        const auto assetPath = resolve_content_sibling(*manifestPath, metadata.relativeFile.generic_string(), &pathError);
        if (!assetPath) {
            return failContent({ContentErrorCode::InvalidPath, pathError, metadata.relativeFile.generic_string()}, false,
                               metadata.id, "invalid object asset path");
        }
        item.assetPath = *assetPath;
        auto bytes = content.read(item.assetPath, &readError, options.limits.maximumDvoxBytesPerObject);
        if (!bytes) return failContent(readError, false, metadata.id, "could not read DVOX asset for '" + metadata.name + "'");
        DvoxReadResult read = read_dvox(*bytes, options.limits.maximumDvoxBytesPerObject);
        if (!read.success) {
            result.error = {RuntimeSceneErrorCode::DvoxReadFailed, "DVOX load failed: " + read.error, item.assetPath, metadata.id};
            return result;
        }
        RuntimeSceneError assetError = validate_dvoxscene_asset(metadata, read.asset, options.limits, item.assetPath);
        if (assetError) {
            result.error = std::move(assetError);
            return result;
        }
        item.asset.emplace(std::move(read.asset));
        item.metadata = std::move(metadata);
        staged.push_back(std::move(item));
    }

    // Publish.
    const auto rollback = [&] {
        for (auto it = result.objects.rbegin(); it != result.objects.rend(); ++it) {
            if (it->gameObjectId != kInvalidGameObjectId) (void)world.destroy_object(it->gameObjectId);
        }
        result.objects.clear();
    };
    try {
        result.objects.reserve(staged.size());
        for (StagedObject& item : staged) {
            const RuntimeSceneObjectMetadata& metadata = item.metadata;
            GameSceneLoadedObject loaded;
            loaded.index = metadata.index;
            loaded.sceneObjectId = metadata.id;
            loaded.name = metadata.name;
            loaded.assetPath = item.assetPath;
            loaded.anchored = metadata.anchored;
            loaded.collision = metadata.generateCollision;
            loaded.attached = options.attachChildrenToParents && metadata.parentIndex.has_value();
            const bool dynamic = !metadata.anchored || loaded.attached;
            std::string spawnError;
            if (item.asset && !metadata.generateCollision) {
                loaded.gameObjectId = world.spawn_visual_asset(
                    std::move(*item.asset), metadata.name, metadata.worldTransform, &spawnError);
            } else if (item.asset) {
                loaded.gameObjectId = world.spawn_cooked_asset(
                    std::move(*item.asset), metadata.name, metadata.worldTransform, dynamic, metadata.structural, &spawnError);
            } else {
                GameObjectDesc desc;
                desc.name = metadata.name;
                desc.transform = metadata.worldTransform;
                desc.dynamic = false;
                desc.structural = metadata.structural;
                loaded.gameObjectId = world.create_object(std::move(desc), &spawnError);
            }
            if (loaded.gameObjectId == kInvalidGameObjectId) {
                rollback();
                result.error = {RuntimeSceneErrorCode::PhysicsPublicationFailure,
                                "could not create runtime object '" + metadata.name + "': " + spawnError,
                                item.assetPath, metadata.id};
                return result;
            }
            result.objects.push_back(std::move(loaded));
        }
        for (std::size_t i = 0; i < staged.size(); ++i) {
            if (!staged[i].metadata.parentIndex) continue;
            GameSceneLoadedObject& child = result.objects[i];
            child.parentGameObjectId = result.objects[*staged[i].metadata.parentIndex].gameObjectId;
            if (!child.attached) continue;
            std::string attachError;
            if (!world.attach_object(child.gameObjectId, *child.parentGameObjectId, true, {}, true, true, &attachError)) {
                const std::string name = child.name;
                const std::uint64_t id = child.sceneObjectId;
                rollback();
                result.error = {RuntimeSceneErrorCode::InvalidParent,
                                "could not attach '" + name + "' to its parent: " + attachError, {}, id};
                return result;
            }
        }
    } catch (const std::exception& exception) {
        rollback();
        result.error = {RuntimeSceneErrorCode::PhysicsPublicationFailure,
                        std::string("scene publication raised: ") + exception.what(), {}, std::nullopt};
        return result;
    }
    return result;
}

} // namespace dve
