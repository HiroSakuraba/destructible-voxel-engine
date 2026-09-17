#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "dve/gabor_volume.hpp"
#include "dve/rhi/device.hpp"
#include "dve/runtime_gabor_volume_world.hpp"

namespace dve::render {

struct alignas(16) GpuGaborPrimitive {
    Float3 center{}; float opacity{};
    Float3 scale{}; float frequency{};
    Quaternion rotation{};
    Float3 albedo{}; float extent{};
    std::uint32_t lodLevel{};
    std::uint32_t orientationBin{};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
};
static_assert(sizeof(GpuGaborPrimitive) == 80U);

struct GaborGpuAsset {
    std::uint64_t assetId{};
    std::uint64_t contentHash{};
    rhi::BufferHandle primitiveBuffer;
    rhi::BufferHandle levelRangeBuffer;
    std::uint32_t primitiveCount{};
    std::uint32_t levelCount{};
    std::size_t residentBytes{};
};

struct GaborGpuCacheStats {
    std::uint64_t uploads{};
    std::uint64_t replacements{};
    std::uint64_t unchanged{};
    std::uint64_t failures{};
    std::uint64_t removals{};
    std::size_t residentAssets{};
    std::size_t residentBytes{};
};

class GaborGpuCache {
public:
    explicit GaborGpuCache(rhi::IDevice& device) : device_(device) {}
    ~GaborGpuCache();
    GaborGpuCache(const GaborGpuCache&) = delete;
    GaborGpuCache& operator=(const GaborGpuCache&) = delete;

    bool upload(std::uint64_t assetId, const GaborVolumeAsset& asset,
                std::string* error = nullptr);
    [[nodiscard]] const GaborGpuAsset* find(std::uint64_t assetId) const noexcept;
    bool remove(std::uint64_t assetId, std::string* error = nullptr);
    void clear() noexcept;
    [[nodiscard]] GaborGpuCacheStats stats() const noexcept;

private:
    bool destroy(GaborGpuAsset& asset, std::string* error) noexcept;
    rhi::IDevice& device_;
    std::unordered_map<std::uint64_t, GaborGpuAsset> assets_;
    GaborGpuCacheStats stats_{};
};

enum class GaborVolumePassKind : std::uint8_t {
    BuildTileLists,
    Integrate,
    ResolveTemporal,
    CastVolumeShadows,
    Composite,
};

struct GaborVolumeDispatch {
    GaborVolumePassKind pass{GaborVolumePassKind::Integrate};
    std::uint32_t groupsX{1};
    std::uint32_t groupsY{1};
    std::uint32_t groupsZ{1};
};

struct GaborVolumeRenderPacket {
    std::uint64_t objectId{};
    std::uint64_t assetId{};
    RigidTransform transform{};
    const GaborGpuAsset* gpu{};
    GaborVolumeFramePlan assetPlan{};
    bool selected{};
};

struct GaborVolumeHistoryState {
    bool valid{};
    std::uint64_t cameraGeneration{};
    std::uint64_t sceneRevision{};
    std::uint64_t settingsHash{};
};

struct GaborVolumeRenderPlan {
    bool enabled{};
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    std::uint32_t tileSize{8};
    std::uint32_t tileCount{};
    std::uint32_t primitiveCapacityPerTile{};
    std::uint64_t submittedPrimitives{};
    std::uint64_t culledPrimitives{};
    std::uint64_t missingAssets{};
    std::uint64_t estimatedTileReferences{};
    std::uint64_t overflowReferences{};
    bool usesSceneDepth{true};
    bool compositeBeforePostProcessing{true};
    bool temporalHistoryRead{};
    bool temporalHistoryWrite{};
    bool temporalHistoryReset{};
    bool volumeShadowWork{};
    bool sceneShadowReception{};
    std::vector<GaborVolumeRenderPacket> packets;
    std::vector<GaborVolumeDispatch> dispatches;

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] GaborVolumeRenderPlan make_gabor_volume_render_plan(
    const RuntimeGaborVolumeSceneSnapshot& snapshot,
    const GaborGpuCache& cache,
    const GaborVolumeRenderSettings& settings,
    std::uint32_t viewportWidth,
    std::uint32_t viewportHeight,
    float projectedDiameterPixels,
    std::uint64_t cameraGeneration,
    GaborVolumeHistoryState* history = nullptr);

struct GaborVolumePipelines {
    rhi::ComputePipelineHandle buildTileLists;
    rhi::ComputePipelineHandle integrate;
    rhi::ComputePipelineHandle resolveTemporal;
    rhi::ComputePipelineHandle castVolumeShadows;
    rhi::ComputePipelineHandle composite;
};

bool record_gabor_volume_frame(
    rhi::IDevice& device,
    rhi::CommandListHandle commands,
    const GaborVolumePipelines& pipelines,
    const GaborVolumeRenderPlan& plan,
    std::string* error = nullptr);

} // namespace dve::render
