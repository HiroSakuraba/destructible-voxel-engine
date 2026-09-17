#include "dve/runtime_gabor_volume_world.hpp"

#include <algorithm>
#include <set>

namespace dve {
namespace {
void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}
}

bool RuntimeGaborVolumeSceneSnapshot::validate(std::string* error) const {
    std::set<std::uint64_t> assetIds;
    for (const auto& asset : assets) {
        std::string validation;
        if (!asset.validate(&validation)) {
            set_error(error, "invalid Gabor asset in snapshot: " + validation);
            return false;
        }
        if (!assetIds.insert(asset.contentHash).second) {
            set_error(error, "snapshot contains duplicate Gabor asset hashes");
            return false;
        }
    }
    std::set<std::uint64_t> objectIds;
    for (const auto& instance : instances) {
        if (instance.objectId == 0 || instance.assetId == 0 ||
            !objectIds.insert(instance.objectId).second) {
            set_error(error, "snapshot contains invalid or duplicate Gabor object IDs");
            return false;
        }
        if (!assetIds.contains(instance.assetId)) {
            set_error(error, "snapshot instance references an unavailable Gabor asset");
            return false;
        }
    }
    return true;
}

RuntimeGaborVolumeObjectId RuntimeGaborVolumeWorld::create(
    GaborVolumeAsset asset, RigidTransform transform,
    RuntimeGaborVolumeObjectId requestedObjectId,
    RuntimeGaborVolumeAssetId requestedAssetId, std::string* error) {
    asset.recompute_bounds_and_hash();
    std::string validation;
    if (!asset.validate(&validation)) {
        set_error(error, validation);
        return 0;
    }
    RuntimeGaborVolumeObjectId objectId = requestedObjectId;
    if (objectId == 0) {
        while (nextObjectId_ == 0 || instances_.contains(nextObjectId_)) ++nextObjectId_;
        objectId = nextObjectId_++;
    }
    if (instances_.contains(objectId)) {
        set_error(error, "duplicate runtime Gabor object ID");
        return 0;
    }
    if (asset.contentHash == 0) {
        set_error(error, "runtime Gabor asset content hash must be nonzero");
        return 0;
    }
    if (requestedAssetId != 0 && requestedAssetId != asset.contentHash) {
        set_error(error, "requested runtime Gabor asset ID must equal the canonical content hash");
        return 0;
    }
    const RuntimeGaborVolumeAssetId assetId = asset.contentHash;
    if (const auto existing = assets_.find(assetId); existing != assets_.end() &&
        existing->second.contentHash != asset.contentHash) {
        set_error(error, "runtime Gabor asset ID collides with different content");
        return 0;
    }
    assets_.try_emplace(assetId, std::move(asset));
    ++assetUsers_[assetId];
    instances_.emplace(objectId, RuntimeGaborVolumeInstance{
        objectId, assetId, transform, true, false, true, true, true});
    nextObjectId_ = std::max(nextObjectId_, objectId + 1U);
    ++revision_;
    return objectId;
}

bool RuntimeGaborVolumeWorld::update_asset(RuntimeGaborVolumeObjectId objectId,
                                            GaborVolumeAsset asset,
                                            std::string* error) {
    auto instance = instances_.find(objectId);
    if (instance == instances_.end()) {
        set_error(error, "runtime Gabor object does not exist");
        return false;
    }
    asset.recompute_bounds_and_hash();
    std::string validation;
    if (!asset.validate(&validation)) {
        set_error(error, validation);
        return false;
    }
    if (asset.contentHash == 0) {
        set_error(error, "runtime Gabor asset content hash must be nonzero");
        return false;
    }
    const RuntimeGaborVolumeAssetId newAssetId = asset.contentHash;
    if (const auto existing = assets_.find(newAssetId); existing != assets_.end() &&
        existing->second.contentHash != asset.contentHash) {
        set_error(error, "runtime Gabor asset ID collides with different content");
        return false;
    }
    const RuntimeGaborVolumeAssetId oldAssetId = instance->second.assetId;
    assets_.try_emplace(newAssetId, std::move(asset));
    ++assetUsers_[newAssetId];
    instance->second.assetId = newAssetId;
    if (auto users = assetUsers_.find(oldAssetId); users != assetUsers_.end()) {
        if (--users->second == 0U) {
            assetUsers_.erase(users);
            assets_.erase(oldAssetId);
        }
    }
    ++revision_;
    return true;
}

bool RuntimeGaborVolumeWorld::update_instance(
    RuntimeGaborVolumeObjectId objectId,
    const RuntimeGaborVolumeInstance& replacement,
    std::string* error) {
    auto instance = instances_.find(objectId);
    if (instance == instances_.end()) {
        set_error(error, "runtime Gabor object does not exist");
        return false;
    }
    if (replacement.objectId != objectId || replacement.assetId != instance->second.assetId) {
        set_error(error, "runtime Gabor instance identity cannot be changed by update_instance");
        return false;
    }
    instance->second = replacement;
    ++revision_;
    return true;
}

bool RuntimeGaborVolumeWorld::destroy(RuntimeGaborVolumeObjectId objectId) noexcept {
    const auto instance = instances_.find(objectId);
    if (instance == instances_.end()) return false;
    const RuntimeGaborVolumeAssetId assetId = instance->second.assetId;
    instances_.erase(instance);
    if (auto users = assetUsers_.find(assetId); users != assetUsers_.end()) {
        if (--users->second == 0U) {
            assetUsers_.erase(users);
            assets_.erase(assetId);
        }
    }
    ++revision_;
    return true;
}

const RuntimeGaborVolumeInstance* RuntimeGaborVolumeWorld::find_instance(
    RuntimeGaborVolumeObjectId objectId) const noexcept {
    const auto iterator = instances_.find(objectId);
    return iterator == instances_.end() ? nullptr : &iterator->second;
}
const GaborVolumeAsset* RuntimeGaborVolumeWorld::find_asset(
    RuntimeGaborVolumeAssetId assetId) const noexcept {
    const auto iterator = assets_.find(assetId);
    return iterator == assets_.end() ? nullptr : &iterator->second;
}

RuntimeGaborVolumeSceneSnapshot RuntimeGaborVolumeWorld::snapshot() const {
    RuntimeGaborVolumeSceneSnapshot result;
    result.revision = revision_;
    result.assets.reserve(assets_.size());
    result.instances.reserve(instances_.size());
    for (const auto& [id, asset] : assets_) {
        (void)id;
        result.assets.push_back(asset);
    }
    for (const auto& [id, instance] : instances_) {
        (void)id;
        result.instances.push_back(instance);
    }
    return result;
}

} // namespace dve
