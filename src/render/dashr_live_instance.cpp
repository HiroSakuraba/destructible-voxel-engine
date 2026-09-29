#include "dve/render/dashr_live_instance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace dve::render {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool finite(float value) noexcept { return std::isfinite(value); }

} // namespace

DashrLiveSurfaceInstance::DashrLiveSurfaceInstance(rhi::IDevice& device)
    : device_(device) {}

DashrLiveSurfaceInstance::~DashrLiveSurfaceInstance() {
    std::string ignored;
    (void)reset(&ignored);
}

bool DashrLiveSurfaceInstance::validate_config(
    const CookedPolygonAsset& asset,
    const DashrLiveSurfaceConfig& config,
    std::string* error) const {
    const auto validAsset = validate_polygon_asset(asset);
    if (!validAsset) {
        set_error(error, validAsset.message);
        return false;
    }
    if (config.atlasResolution < 32U || config.atlasResolution > 2048U) {
        set_error(error, "DASHR live atlas resolution must be in [32, 2048]");
        return false;
    }
    if (config.materials.empty()) {
        set_error(error, "DASHR live instance requires at least one material");
        return false;
    }

    std::unordered_set<std::uint32_t> unique;
    for (const auto& material : config.materials) {
        if (material.materialIndex >= asset.materialBindings.size() ||
            material.materialIndex >= asset.materials.size()) {
            set_error(error, "DASHR live material index is out of range");
            return false;
        }
        if (!unique.insert(material.materialIndex).second) {
            set_error(error, "DASHR live material index is duplicated");
            return false;
        }
        std::string settingsError;
        if (!validate_dashr_surface_settings(material.traceSettings, &settingsError)) {
            set_error(error, settingsError);
            return false;
        }
        const auto& binding = asset.materialBindings[material.materialIndex];
        if (!binding.height.texture) {
            set_error(error, "DASHR live material requires a height texture");
            return false;
        }
        if (binding.height.texcoord != 0U) {
            set_error(error, "DASHR live surface atlas currently requires height UV0");
            return false;
        }
        if (binding.mapping.mappingMode != MaterialMappingMode::UV0) {
            set_error(error, "DASHR live surface atlas currently requires UV0 mapping");
            return false;
        }
        // The camera PBR shader already supports masked materials, but the first
        // displaced shadow shader does not yet evaluate opacity. Reject rather
        // than publishing a camera/shadow mismatch.
        if (asset.materials[material.materialIndex].blendMode == MaterialBlendMode::Masked) {
            set_error(error,
                "DASHR live masked materials are disabled until displaced shadow alpha parity is implemented");
            return false;
        }
    }
    return true;
}

DashrSurfaceSettings DashrLiveSurfaceInstance::shell_envelope_settings() const noexcept {
    DashrSurfaceSettings result = materials_.front().traceSettings;
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();
    float padding = 0.0F;
    for (const auto& material : materials_) {
        const auto& settings = material.traceSettings;
        const float low = (0.0F - settings.heightReferencePlane) * settings.heightScale +
                          settings.heightOffset;
        const float high = (1.0F - settings.heightReferencePlane) * settings.heightScale +
                           settings.heightOffset;
        minimum = std::min(minimum, std::min(low, high));
        maximum = std::max(maximum, std::max(low, high));
        padding = std::max(padding, settings.envelopePadding);
    }
    if (!finite(minimum) || !finite(maximum) || maximum < minimum) return result;

    // build_dashr_shell_mesh only needs a conservative height interval. Encode
    // the union [minimum, maximum] into a synthetic 0..1 mapping centered on 0.5.
    result.heightScale = maximum - minimum;
    result.heightReferencePlane = 0.5F;
    result.heightOffset = (minimum + maximum) * 0.5F;
    result.envelopePadding = padding;
    return result;
}

bool DashrLiveSurfaceInstance::wait_for_safe_reuse(bool& waited, std::string* error) {
    waited = false;
    if (!lastGpuFence_ || device_.fence_complete(lastGpuFence_)) return true;
    waited = true;
    return device_.wait(lastGpuFence_, error);
}

bool DashrLiveSurfaceInstance::publish_pose(
    const CookedPolygonAsset& asset,
    std::span<const Float3> deformedPositions,
    std::uint64_t poseRevision,
    DashrLiveSurfaceUpdateStats& stats,
    std::string* error) {
    if (!surface_ || !shell_ || !atlas_.valid()) {
        set_error(error, "DASHR live instance is not initialized");
        return false;
    }
    if (!deformedPositions.empty() && deformedPositions.size() != asset.vertices.size()) {
        set_error(error, "DASHR live pose vertex count does not match the polygon asset");
        return false;
    }

    if (!surface_->upload(asset, deformedPositions, &stats.surfaceBuild, error)) return false;

    const DashrSurfaceSettings envelope = shell_envelope_settings();
    auto shellMesh = build_dashr_shell_mesh(asset, envelope, deformedPositions, error);
    if (!shellMesh) return false;
    stats.shellBuild = shellMesh->stats;
    if (!shell_->upload(*shellMesh, error)) return false;

    rhi::FenceHandle atlasFence;
    if (!record_dashr_atlas_update(
            device_, atlas_, *surface_, asset, stats.atlas, &atlasFence, error)) return false;
    lastGpuFence_ = atlasFence;
    poseRevision_ = poseRevision;
    stats.poseRevision = poseRevision;
    return true;
}

bool DashrLiveSurfaceInstance::initialize(
    const CookedPolygonAsset& asset,
    const DashrAtlasShaderBytecode& bytecode,
    const DashrLiveSurfaceConfig& config,
    std::span<const Float3> initialDeformedPositions,
    std::uint64_t poseRevision,
    DashrLiveSurfaceUpdateStats* stats,
    std::string* error) {
    if (!bytecode.valid()) {
        set_error(error, "DASHR live atlas shader bytecode is incomplete");
        return false;
    }
    if (!validate_config(asset, config, error)) return false;

    std::string local;
    if (!reset(&local)) {
        set_error(error, local);
        return false;
    }

    materials_ = config.materials;
    assetContentHash_ = asset.contentHash;
    surface_ = std::make_unique<DashrSurfaceMeshMirror>(device_);
    shell_ = std::make_unique<DashrShellMeshMirror>(device_);

    if (!create_dashr_atlas_resources(
            device_, bytecode, config.atlasResolution, atlas_, &local)) {
        (void)reset(nullptr);
        set_error(error, local);
        return false;
    }

    DashrSeamCookSettings seam = config.seamCook;
    seam.resolution = config.atlasResolution;
    auto seamMap = cook_dashr_seam_map(asset, seam, &local);
    if (!seamMap || !upload_dashr_seam_map(device_, atlas_, seamMap->texels, &local)) {
        (void)reset(nullptr);
        set_error(error, local);
        return false;
    }

    DashrLiveSurfaceUpdateStats localStats;
    if (!publish_pose(asset, initialDeformedPositions, poseRevision, localStats, &local)) {
        (void)reset(nullptr);
        set_error(error, local);
        return false;
    }
    if (stats) *stats = localStats;
    return true;
}

bool DashrLiveSurfaceInstance::update_pose(
    const CookedPolygonAsset& asset,
    std::span<const Float3> deformedPositions,
    std::uint64_t poseRevision,
    DashrLiveSurfaceUpdateStats* stats,
    std::string* error) {
    DashrLiveSurfaceUpdateStats localStats;
    localStats.poseRevision = poseRevision_;
    if (!ready() || asset.contentHash != assetContentHash_) {
        set_error(error, "DASHR live pose update does not match the initialized asset");
        return false;
    }
    if (poseRevision == poseRevision_) {
        localStats.skippedUnchangedPose = true;
        if (stats) *stats = localStats;
        return true;
    }
    if (poseRevision < poseRevision_) {
        set_error(error, "DASHR live pose revision moved backwards");
        return false;
    }
    if (!wait_for_safe_reuse(localStats.waitedForGpu, error)) return false;
    if (!publish_pose(asset, deformedPositions, poseRevision, localStats, error)) return false;
    if (stats) *stats = localStats;
    return true;
}

void DashrLiveSurfaceInstance::mark_gpu_use(rhi::FenceHandle fence) noexcept {
    if (fence) lastGpuFence_ = fence;
}

bool DashrLiveSurfaceInstance::reset(std::string* error) {
    bool ok = true;
    std::string local;
    if (lastGpuFence_ && !device_.fence_complete(lastGpuFence_)) {
        if (!device_.wait(lastGpuFence_, &local)) {
            ok = false;
            if (error && error->empty()) *error = local;
        }
    }
    local.clear();
    if (atlas_.valid() && !destroy_dashr_atlas_resources(device_, atlas_, &local)) {
        ok = false;
        if (error && error->empty()) *error = local;
    }
    atlas_ = {};
    shell_.reset();
    surface_.reset();
    materials_.clear();
    assetContentHash_ = 0U;
    poseRevision_ = 0U;
    lastGpuFence_ = {};
    return ok;
}

bool DashrLiveSurfaceInstance::ready() const noexcept {
    return assetContentHash_ != 0U && !materials_.empty() && surface_ && shell_ &&
           surface_->vertex_buffer() && shell_->vertex_buffer() && atlas_.valid() && atlas_.published;
}

bool DashrLiveSurfaceInstance::uses_material(std::uint32_t materialIndex) const noexcept {
    return material_settings(materialIndex) != nullptr;
}

const DashrSurfaceSettings* DashrLiveSurfaceInstance::material_settings(
    std::uint32_t materialIndex) const noexcept {
    for (const auto& material : materials_)
        if (material.materialIndex == materialIndex) return &material.traceSettings;
    return nullptr;
}

} // namespace dve::render
