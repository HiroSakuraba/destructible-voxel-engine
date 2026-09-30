#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "dve/dashr_seam_map.hpp"
#include "dve/render/dashr_atlas.hpp"
#include "dve/render/dashr_shell.hpp"

namespace dve::render {

struct DashrPoseInput {
    std::span<const Float3> objectSpacePositions{};
    std::uint64_t revision{};
};

// One object-space pose and one UV atlas per instance. A failed update invalidates
// publication; callers must never render the shell with an older atlas revision.
class DashrLiveSurfaceInstance {
public:
    explicit DashrLiveSurfaceInstance(rhi::IDevice& device);
    ~DashrLiveSurfaceInstance();
    DashrLiveSurfaceInstance(const DashrLiveSurfaceInstance&) = delete;
    DashrLiveSurfaceInstance& operator=(const DashrLiveSurfaceInstance&) = delete;

    [[nodiscard]] bool initialize(const CookedPolygonAsset& asset,
                                  const DashrSurfaceSettings& settings,
                                  const DashrAtlasShaderBytecode& bytecode,
                                  std::uint32_t atlasResolution,
                                  std::string* error = nullptr);
    [[nodiscard]] bool update_pose(DashrPoseInput pose, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool published() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> published_revision() const noexcept {
        return publishedRevision_;
    }
    [[nodiscard]] std::uint64_t asset_content_hash() const noexcept { return assetContentHash_; }
    [[nodiscard]] const DashrAtlasResources& atlas() const noexcept { return atlas_; }
    [[nodiscard]] const DashrShellMeshMirror& shell() const noexcept { return shell_; }
    [[nodiscard]] const DashrSurfaceMeshMirror& surface() const noexcept { return surface_; }
    [[nodiscard]] const DashrSurfaceSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] std::uint64_t atlas_updates() const noexcept { return atlasUpdates_; }
    [[nodiscard]] std::uint64_t skipped_updates() const noexcept { return skippedUpdates_; }

private:
    rhi::IDevice& device_;
    const CookedPolygonAsset* asset_{};
    DashrSurfaceSettings settings_{};
    DashrSurfaceMeshMirror surface_;
    DashrAtlasResources atlas_{};
    DashrShellMeshMirror shell_;
    std::uint64_t assetContentHash_{};
    std::optional<std::uint64_t> publishedRevision_{};
    std::uint64_t atlasUpdates_{};
    std::uint64_t skippedUpdates_{};
};

} // namespace dve::render
