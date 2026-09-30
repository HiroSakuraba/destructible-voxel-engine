#include "dve/render/dashr_live_surface.hpp"

#include <utility>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}
}

DashrLiveSurfaceInstance::DashrLiveSurfaceInstance(rhi::IDevice& device)
    : device_(device), surface_(device), shell_(device) {}

DashrLiveSurfaceInstance::~DashrLiveSurfaceInstance() { reset(); }

bool DashrLiveSurfaceInstance::initialize(const CookedPolygonAsset& asset,
                                          const DashrSurfaceSettings& settings,
                                          const DashrAtlasShaderBytecode& bytecode,
                                          std::uint32_t atlasResolution,
                                          std::string* error) {
    reset();
    if (!validate_polygon_asset(asset) || !validate_dashr_surface_settings(settings, error)) {
        set_error(error, "DASHR live surface requires a valid cooked asset and settings");
        return false;
    }
    if (!create_dashr_atlas_resources(device_, bytecode, atlasResolution, atlas_, error))
        return false;
    DashrSeamCookSettings seamSettings;
    seamSettings.resolution = atlasResolution;
    auto seam = cook_dashr_seam_map(asset, seamSettings, error);
    if (!seam || !upload_dashr_seam_map(device_, atlas_, seam->texels, error)) {
        reset();
        return false;
    }
    asset_ = &asset;
    assetContentHash_ = asset.contentHash;
    settings_ = settings;
    return true;
}

bool DashrLiveSurfaceInstance::update_pose(DashrPoseInput pose, std::string* error) {
    if (!asset_ || !atlas_.valid() || asset_->contentHash != assetContentHash_) {
        set_error(error, "DASHR live surface is uninitialized or its asset changed");
        return false;
    }
    if (publishedRevision_ && *publishedRevision_ == pose.revision) {
        ++skippedUpdates_;
        return true;
    }
    if (!pose.objectSpacePositions.empty() &&
        pose.objectSpacePositions.size() != asset_->vertices.size()) {
        set_error(error, "DASHR pose position count does not match the asset");
        return false;
    }

    // The correctness baseline serializes reuse of both mesh buffers and atlas
    // images. Buffered resource slots can replace this wait after GPU validation.
    device_.wait_idle();
    publishedRevision_.reset();
    if (!surface_.upload(*asset_, pose.objectSpacePositions, nullptr, error)) {
        reset();
        return false;
    }
    const auto built = build_dashr_shell_mesh(*asset_, settings_, pose.objectSpacePositions, error);
    if (!built || !shell_.upload(*built, error)) {
        reset();
        return false;
    }
    DashrAtlasUpdateStats stats;
    if (!record_dashr_atlas_update(device_, atlas_, surface_, *asset_, stats, nullptr, error)) {
        reset();
        return false;
    }
    publishedRevision_ = pose.revision;
    ++atlasUpdates_;
    return true;
}

bool DashrLiveSurfaceInstance::published() const noexcept {
    return asset_ && asset_->contentHash == assetContentHash_ &&
           publishedRevision_.has_value() && atlas_.published &&
           shell_.vertex_buffer() && shell_.index_buffer();
}

void DashrLiveSurfaceInstance::reset() noexcept {
    device_.wait_idle();
    shell_.reset();
    surface_.reset();
    if (atlas_.valid()) {
        std::string ignored;
        (void)destroy_dashr_atlas_resources(device_, atlas_, &ignored);
    }
    asset_ = nullptr;
    assetContentHash_ = 0U;
    publishedRevision_.reset();
    atlasUpdates_ = skippedUpdates_ = 0U;
}

} // namespace dve::render
