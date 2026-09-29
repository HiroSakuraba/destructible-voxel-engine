#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dve/dashr_seam_map.hpp"
#include "dve/dashr_surface.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/render/dashr_atlas.hpp"
#include "dve/render/dashr_shell.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

// Runtime opt-in for one material in a polygon asset. Keeping the selection on the
// instance avoids silently changing existing parallax materials while the DASHR
// material mode is being integrated into DVE's persistent asset format.
struct DashrLiveMaterialConfig {
    std::uint32_t materialIndex{};
    DashrSurfaceSettings traceSettings{};
};

struct DashrLiveSurfaceConfig {
    std::uint32_t atlasResolution{512U};
    DashrSeamCookSettings seamCook{};
    std::vector<DashrLiveMaterialConfig> materials;
};

struct DashrLiveSurfaceUpdateStats {
    DashrSurfaceMeshBuildStats surfaceBuild{};
    DashrAtlasUpdateStats atlas{};
    DashrShellBuildStats shellBuild{};
    std::uint64_t poseRevision{};
    bool waitedForGpu{};
    bool skippedUnchangedPose{};
};

// Owns all pose-dependent DASHR resources for one rendered polygon instance.
//
// The first implementation intentionally serializes writes against the most
// recent GPU use fence. This prevents a frame-N+1 CPU pose upload from mutating
// the surface/shell buffers or atlas while frame N is still reading them. Once
// the path is visually validated this can be replaced by two/three buffered
// pose resource sets without changing LiveEnvironmentRenderer's scheduling API.
class DashrLiveSurfaceInstance {
public:
    explicit DashrLiveSurfaceInstance(rhi::IDevice& device);
    ~DashrLiveSurfaceInstance();
    DashrLiveSurfaceInstance(const DashrLiveSurfaceInstance&) = delete;
    DashrLiveSurfaceInstance& operator=(const DashrLiveSurfaceInstance&) = delete;

    [[nodiscard]] bool initialize(
        const CookedPolygonAsset& asset,
        const DashrAtlasShaderBytecode& bytecode,
        const DashrLiveSurfaceConfig& config,
        std::span<const Float3> initialDeformedPositions = {},
        std::uint64_t poseRevision = 1U,
        DashrLiveSurfaceUpdateStats* stats = nullptr,
        std::string* error = nullptr);

    [[nodiscard]] bool update_pose(
        const CookedPolygonAsset& asset,
        std::span<const Float3> deformedPositions,
        std::uint64_t poseRevision,
        DashrLiveSurfaceUpdateStats* stats = nullptr,
        std::string* error = nullptr);

    // Record the most recent submission that reads this instance. The renderer
    // calls this after displaced shadow and camera submissions. A later pose
    // update will not mutate the resources until this fence is complete.
    void mark_gpu_use(rhi::FenceHandle fence) noexcept;

    [[nodiscard]] bool reset(std::string* error = nullptr);

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool uses_material(std::uint32_t materialIndex) const noexcept;
    [[nodiscard]] const DashrSurfaceSettings* material_settings(
        std::uint32_t materialIndex) const noexcept;
    [[nodiscard]] std::uint64_t asset_content_hash() const noexcept {
        return assetContentHash_;
    }
    [[nodiscard]] std::uint64_t pose_revision() const noexcept { return poseRevision_; }
    [[nodiscard]] std::uint32_t atlas_resolution() const noexcept { return atlas_.resolution; }
    [[nodiscard]] const DashrAtlasResources& atlas() const noexcept { return atlas_; }
    [[nodiscard]] const DashrShellMeshMirror* shell() const noexcept { return shell_.get(); }
    [[nodiscard]] const DashrSurfaceMeshMirror* surface() const noexcept { return surface_.get(); }
    [[nodiscard]] rhi::FenceHandle last_gpu_fence() const noexcept { return lastGpuFence_; }

private:
    [[nodiscard]] bool validate_config(
        const CookedPolygonAsset& asset,
        const DashrLiveSurfaceConfig& config,
        std::string* error) const;
    [[nodiscard]] bool wait_for_safe_reuse(bool& waited, std::string* error);
    [[nodiscard]] bool publish_pose(
        const CookedPolygonAsset& asset,
        std::span<const Float3> deformedPositions,
        std::uint64_t poseRevision,
        DashrLiveSurfaceUpdateStats& stats,
        std::string* error);
    [[nodiscard]] DashrSurfaceSettings shell_envelope_settings() const noexcept;

    rhi::IDevice& device_;
    std::unique_ptr<DashrSurfaceMeshMirror> surface_;
    std::unique_ptr<DashrShellMeshMirror> shell_;
    DashrAtlasResources atlas_{};
    std::vector<DashrLiveMaterialConfig> materials_;
    std::uint64_t assetContentHash_{};
    std::uint64_t poseRevision_{};
    rhi::FenceHandle lastGpuFence_{};
};

} // namespace dve::render
