#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dve/gabor_volume.hpp"
#include "dve/transform.hpp"

namespace dve {

using RuntimeGaborVolumeObjectId = std::uint64_t;
using RuntimeGaborVolumeAssetId = std::uint64_t;

struct RuntimeGaborVolumeInstance {
    RuntimeGaborVolumeObjectId objectId{};
    RuntimeGaborVolumeAssetId assetId{};
    RigidTransform worldTransform{};
    bool visible{true};
    bool selected{};
    bool castShadows{true};
    bool receiveSceneShadows{true};
    bool temporalAccumulation{true};
};

struct RuntimeGaborVolumeSceneSnapshot {
    std::vector<GaborVolumeAsset> assets;
    std::vector<RuntimeGaborVolumeInstance> instances;
    std::uint64_t revision{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

class RuntimeGaborVolumeWorld {
public:
    [[nodiscard]] RuntimeGaborVolumeObjectId create(
        GaborVolumeAsset asset,
        RigidTransform transform = {},
        RuntimeGaborVolumeObjectId requestedObjectId = 0,
        RuntimeGaborVolumeAssetId requestedAssetId = 0,
        std::string* error = nullptr);
    bool update_asset(RuntimeGaborVolumeObjectId objectId, GaborVolumeAsset asset,
                      std::string* error = nullptr);
    bool update_instance(RuntimeGaborVolumeObjectId objectId,
                         const RuntimeGaborVolumeInstance& instance,
                         std::string* error = nullptr);
    bool destroy(RuntimeGaborVolumeObjectId objectId) noexcept;
    [[nodiscard]] const RuntimeGaborVolumeInstance* find_instance(
        RuntimeGaborVolumeObjectId objectId) const noexcept;
    [[nodiscard]] const GaborVolumeAsset* find_asset(
        RuntimeGaborVolumeAssetId assetId) const noexcept;
    [[nodiscard]] RuntimeGaborVolumeSceneSnapshot snapshot() const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    RuntimeGaborVolumeObjectId nextObjectId_{1};
    std::map<RuntimeGaborVolumeObjectId, RuntimeGaborVolumeInstance> instances_;
    std::map<RuntimeGaborVolumeAssetId, GaborVolumeAsset> assets_;
    std::map<RuntimeGaborVolumeAssetId, std::size_t> assetUsers_;
    std::uint64_t revision_{};
};

} // namespace dve
