#include "dve/render/gabor_volume_renderer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <map>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] std::vector<GpuGaborPrimitive> pack_primitives(const GaborVolumeAsset& asset) {
    std::vector<GpuGaborPrimitive> result;
    result.reserve(asset.primitives.size());
    for (const auto& primitive : asset.primitives) {
        result.push_back({primitive.center, primitive.opacity, primitive.scale,
                          primitive.frequency, primitive.rotation, primitive.albedo,
                          primitive.extent, primitive.lodLevel, primitive.orientationBin, 0U, 0U});
    }
    return result;
}

[[nodiscard]] std::vector<std::uint32_t> pack_level_ranges(const GaborVolumeAsset& asset) {
    std::map<std::uint16_t, std::pair<std::uint32_t,std::uint32_t>> ranges;
    for (std::uint32_t index = 0; index < asset.primitives.size(); ++index) {
        const auto level = asset.primitives[index].lodLevel;
        auto [iterator, inserted] = ranges.try_emplace(level, index, 0U);
        if (inserted) iterator->second.first = index;
        ++iterator->second.second;
    }
    std::vector<std::uint32_t> packed;
    packed.reserve(ranges.size()*4U);
    for (const auto& [level, range] : ranges) {
        packed.push_back(level);
        packed.push_back(range.first);
        packed.push_back(range.second);
        packed.push_back(0U);
    }
    return packed;
}

bool upload_buffer(rhi::IDevice& device, rhi::BufferHandle& handle,
                   std::span<const std::byte> bytes, std::string_view name,
                   std::string* error) {
    if (bytes.empty()) { set_error(error, std::string(name)+" is empty"); return false; }
    rhi::BufferDesc desc;
    desc.bytes = bytes.size();
    desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDestination;
    desc.memory = rhi::MemoryDomain::Upload;
    desc.initialState = rhi::ResourceState::ShaderRead;
    desc.debugName = std::string(name);
    handle = device.create_buffer(desc, error);
    return handle && device.write_buffer(handle, 0U, bytes, error);
}

[[nodiscard]] std::uint64_t settings_hash(const GaborVolumeRenderSettings& settings) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    auto append = [&](auto value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    };
    append(settings.enabled); append(settings.mode); append(settings.quality);
    append(settings.continuousLod); append(settings.temporalAccumulation);
    append(settings.castVolumeShadows); append(settings.receiveSceneShadows);
    append(settings.maximumPrimitivesPerTile); append(settings.lodBias);
    return hash;
}

[[nodiscard]] rhi::ComputePipelineHandle pipeline_for(
    GaborVolumePassKind pass, const GaborVolumePipelines& pipelines) noexcept {
    switch (pass) {
        case GaborVolumePassKind::BuildTileLists: return pipelines.buildTileLists;
        case GaborVolumePassKind::Integrate: return pipelines.integrate;
        case GaborVolumePassKind::ResolveTemporal: return pipelines.resolveTemporal;
        case GaborVolumePassKind::CastVolumeShadows: return pipelines.castVolumeShadows;
        case GaborVolumePassKind::Composite: return pipelines.composite;
    }
    return {};
}
}

GaborGpuCache::~GaborGpuCache() { clear(); }

bool GaborGpuCache::destroy(GaborGpuAsset& asset, std::string* error) noexcept {
    bool ok = true;
    if (asset.primitiveBuffer) ok = device_.destroy_buffer(asset.primitiveBuffer, error) && ok;
    if (asset.levelRangeBuffer) ok = device_.destroy_buffer(asset.levelRangeBuffer, error) && ok;
    asset = {};
    return ok;
}

bool GaborGpuCache::upload(std::uint64_t assetId, const GaborVolumeAsset& source,
                           std::string* error) {
    if (assetId == 0) { set_error(error, "Gabor GPU asset ID must be nonzero"); ++stats_.failures; return false; }
    GaborVolumeAsset asset = source;
    asset.recompute_bounds_and_hash();
    std::string validation;
    if (!asset.validate(&validation)) { set_error(error, validation); ++stats_.failures; return false; }
    if (const auto existing = assets_.find(assetId); existing != assets_.end() &&
        existing->second.contentHash == asset.contentHash) {
        ++stats_.unchanged;
        return true;
    }
    const auto primitives = pack_primitives(asset);
    const auto ranges = pack_level_ranges(asset);
    GaborGpuAsset staged;
    staged.assetId = assetId;
    staged.contentHash = asset.contentHash;
    staged.primitiveCount = static_cast<std::uint32_t>(primitives.size());
    staged.levelCount = static_cast<std::uint32_t>(ranges.size()/4U);
    staged.residentBytes = primitives.size()*sizeof(GpuGaborPrimitive) + ranges.size()*sizeof(std::uint32_t);
    if (!upload_buffer(device_, staged.primitiveBuffer,
                       std::as_bytes(std::span<const GpuGaborPrimitive>(primitives)),
                       "Gabor primitives", error) ||
        !upload_buffer(device_, staged.levelRangeBuffer,
                       std::as_bytes(std::span<const std::uint32_t>(ranges)),
                       "Gabor level ranges", error)) {
        (void)destroy(staged, nullptr);
        ++stats_.failures;
        return false;
    }
    if (auto existing = assets_.find(assetId); existing != assets_.end()) {
        GaborGpuAsset old = std::move(existing->second);
        existing->second = std::move(staged);
        (void)destroy(old, nullptr);
        ++stats_.replacements;
    } else {
        assets_.emplace(assetId, std::move(staged));
        ++stats_.uploads;
    }
    return true;
}

const GaborGpuAsset* GaborGpuCache::find(std::uint64_t assetId) const noexcept {
    const auto iterator = assets_.find(assetId);
    return iterator == assets_.end() ? nullptr : &iterator->second;
}
bool GaborGpuCache::remove(std::uint64_t assetId, std::string* error) {
    auto iterator = assets_.find(assetId);
    if (iterator == assets_.end()) return false;
    GaborGpuAsset asset = std::move(iterator->second);
    assets_.erase(iterator);
    ++stats_.removals;
    return destroy(asset, error);
}
void GaborGpuCache::clear() noexcept {
    for (auto& [id, asset] : assets_) { (void)id; (void)destroy(asset, nullptr); }
    assets_.clear();
}
GaborGpuCacheStats GaborGpuCache::stats() const noexcept {
    GaborGpuCacheStats result = stats_;
    result.residentAssets = assets_.size();
    for (const auto& [id, asset] : assets_) { (void)id; result.residentBytes += asset.residentBytes; }
    return result;
}

bool GaborVolumeRenderPlan::validate(std::string* error) const noexcept {
    if (!enabled) return true;
    if (viewportWidth == 0 || viewportHeight == 0 || tileSize == 0 || tileCount == 0) {
        set_error(error, "enabled Gabor frame plan has invalid viewport or tile dimensions");
        return false;
    }
    if (primitiveCapacityPerTile == 0 || packets.empty()) {
        set_error(error, "enabled Gabor frame plan has no capacity or packets");
        return false;
    }
    if (dispatches.empty() || dispatches.front().pass != GaborVolumePassKind::BuildTileLists ||
        dispatches.back().pass != GaborVolumePassKind::Composite) {
        set_error(error, "Gabor frame dispatch ordering is incomplete");
        return false;
    }
    for (const auto& packet : packets) {
        if (packet.objectId == 0 || packet.assetId == 0 || packet.gpu == nullptr ||
            packet.assetPlan.submittedPrimitives == 0) {
            set_error(error, "Gabor frame contains an invalid packet");
            return false;
        }
    }
    return true;
}

GaborVolumeRenderPlan make_gabor_volume_render_plan(
    const RuntimeGaborVolumeSceneSnapshot& snapshot, const GaborGpuCache& cache,
    const GaborVolumeRenderSettings& settings, std::uint32_t viewportWidth,
    std::uint32_t viewportHeight, float projectedDiameterPixels,
    std::uint64_t cameraGeneration, GaborVolumeHistoryState* history) {
    GaborVolumeRenderPlan plan;
    plan.enabled = settings.enabled && viewportWidth > 0 && viewportHeight > 0;
    plan.viewportWidth = viewportWidth;
    plan.viewportHeight = viewportHeight;
    plan.primitiveCapacityPerTile = std::max(1U, settings.maximumPrimitivesPerTile);
    if (!plan.enabled) return plan;
    const std::uint32_t tilesX = (viewportWidth + plan.tileSize - 1U)/plan.tileSize;
    const std::uint32_t tilesY = (viewportHeight + plan.tileSize - 1U)/plan.tileSize;
    plan.tileCount = tilesX*tilesY;

    for (const auto& instance : snapshot.instances) {
        if (!instance.visible) continue;
        const GaborVolumeAsset* asset = nullptr;
        for (const auto& candidate : snapshot.assets) {
            if (candidate.contentHash == instance.assetId) { asset = &candidate; break; }
        }
        const auto* gpu = cache.find(instance.assetId);
        if (!asset || !gpu) { ++plan.missingAssets; continue; }
        GaborVolumeRenderSettings objectSettings = settings;
        objectSettings.castVolumeShadows = objectSettings.castVolumeShadows && instance.castShadows;
        objectSettings.receiveSceneShadows = objectSettings.receiveSceneShadows && instance.receiveSceneShadows;
        objectSettings.temporalAccumulation = objectSettings.temporalAccumulation && instance.temporalAccumulation;
        const auto assetPlan = plan_gabor_volume_frame(*asset, objectSettings, projectedDiameterPixels);
        if (!assetPlan.enabled || assetPlan.submittedPrimitives == 0) continue;
        plan.packets.push_back({instance.objectId, instance.assetId, instance.worldTransform,
                                gpu, assetPlan, instance.selected});
        plan.submittedPrimitives += assetPlan.submittedPrimitives;
        plan.culledPrimitives += assetPlan.culledPrimitives;
        plan.volumeShadowWork = plan.volumeShadowWork ||
            (objectSettings.castVolumeShadows && assetPlan.shadowWork);
        plan.sceneShadowReception = plan.sceneShadowReception || objectSettings.receiveSceneShadows;
    }
    if (plan.packets.empty()) { plan.enabled = false; return plan; }
    plan.estimatedTileReferences = plan.submittedPrimitives *
        std::min<std::uint64_t>(plan.tileCount, 4U);
    const std::uint64_t capacity = static_cast<std::uint64_t>(plan.tileCount)*plan.primitiveCapacityPerTile;
    plan.overflowReferences = plan.estimatedTileReferences > capacity
        ? plan.estimatedTileReferences-capacity : 0U;

    const std::uint64_t currentSettingsHash = settings_hash(settings);
    const bool reset = history == nullptr || !history->valid ||
        history->cameraGeneration != cameraGeneration ||
        history->sceneRevision != snapshot.revision ||
        history->settingsHash != currentSettingsHash;
    plan.temporalHistoryRead = settings.temporalAccumulation && !reset;
    plan.temporalHistoryWrite = settings.temporalAccumulation;
    plan.temporalHistoryReset = settings.temporalAccumulation && reset;
    if (history) {
        history->valid = settings.temporalAccumulation;
        history->cameraGeneration = cameraGeneration;
        history->sceneRevision = snapshot.revision;
        history->settingsHash = currentSettingsHash;
    }

    plan.dispatches.push_back({GaborVolumePassKind::BuildTileLists, tilesX, tilesY, 1U});
    plan.dispatches.push_back({GaborVolumePassKind::Integrate, tilesX, tilesY, 1U});
    if (settings.temporalAccumulation)
        plan.dispatches.push_back({GaborVolumePassKind::ResolveTemporal, tilesX, tilesY, 1U});
    if (plan.volumeShadowWork)
        plan.dispatches.push_back({GaborVolumePassKind::CastVolumeShadows, tilesX, tilesY, 1U});
    plan.dispatches.push_back({GaborVolumePassKind::Composite, tilesX, tilesY, 1U});
    return plan;
}

bool record_gabor_volume_frame(rhi::IDevice& device, rhi::CommandListHandle commands,
                               const GaborVolumePipelines& pipelines,
                               const GaborVolumeRenderPlan& plan,
                               std::string* error) {
    std::string validation;
    if (!plan.validate(&validation)) { set_error(error, validation); return false; }
    if (!plan.enabled) return true;
    for (const auto& dispatch : plan.dispatches) {
        const auto pipeline = pipeline_for(dispatch.pass, pipelines);
        if (!pipeline) {
            set_error(error, "Gabor frame is missing a required compute pipeline");
            return false;
        }
        if (!device.dispatch(commands, pipeline, dispatch.groupsX, dispatch.groupsY,
                             dispatch.groupsZ, error)) return false;
    }
    return true;
}

} // namespace dve::render
