#include "dve/render/dashr_live_surface.hpp"

#include <cmath>
#include <utility>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}
} // namespace

DashrEligibilityResult validate_dashr_submesh(
    const CookedPolygonAsset& asset, std::uint32_t submeshIndex,
    const DashrSurfaceSettings& settings, std::uint32_t atlasResolution) {
    const auto invalid = [](DashrEligibilityCode code, std::string message) {
        return DashrEligibilityResult{code,std::move(message)};
    };
    if (!validate_polygon_asset(asset))
        return invalid(DashrEligibilityCode::InvalidAsset,"DASHR asset is invalid");
    if (submeshIndex >= asset.submeshes.size())
        return invalid(DashrEligibilityCode::InvalidSubmesh,"DASHR submesh index is out of range");
    if (atlasResolution < 32U || atlasResolution > 2048U)
        return invalid(DashrEligibilityCode::InvalidAtlasResolution,
                       "DASHR atlas resolution must be between 32 and 2048");
    std::string settingsError;
    if (!validate_dashr_surface_settings(settings,&settingsError))
        return invalid(DashrEligibilityCode::InvalidTraceSettings,settingsError);
    const auto& submesh = asset.submeshes[submeshIndex];
    const auto& material = asset.materials[submesh.materialIndex];
    const auto& binding = asset.materialBindings[submesh.materialIndex];
    if (material.blendMode != MaterialBlendMode::Opaque)
        return invalid(DashrEligibilityCode::UnsupportedBlendMode,
                       "DASHR currently supports opaque materials only");
    if (binding.mapping.mappingMode != MaterialMappingMode::UV0 || binding.height.texcoord != 0U)
        return invalid(DashrEligibilityCode::UnsupportedUvMode,
                       "DASHR requires UV0 height mapping");
    if (!binding.height.texture)
        return invalid(DashrEligibilityCode::MissingHeightTexture,
                       "DASHR material needs a height texture");
    const auto end = submesh.firstIndex + submesh.indexCount;
    for (std::uint32_t index=submesh.firstIndex; index<end; index+=3U) {
        const auto a = asset.vertices[asset.indices[index]].texcoord;
        const auto b = asset.vertices[asset.indices[index+1U]].texcoord;
        const auto c = asset.vertices[asset.indices[index+2U]].texcoord;
        const float area = (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
        if (!std::isfinite(area) || std::abs(area) < 1.0e-8F)
            return invalid(DashrEligibilityCode::DegenerateUvTriangle,
                           "DASHR submesh contains a degenerate UV triangle");
    }
    return {};
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
