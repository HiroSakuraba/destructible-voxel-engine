#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>

#include "dve/render/camera_frame_graph.hpp"
#include "dve/render/rdv_bbv.hpp"

namespace dve::render::rdv {

// One RHI-backed BBV clipmap per active camera viewport. This is an opt-in
// consumer of CameraFrameGraph::finalize(), not a default renderer replacement.
// Objects in the provided PackedBrickmapScene must already be expressed in
// the same global voxel-index space as the camera position after metresPerVoxel
// conversion. Geometry transforms are outside this initial bridge contract.
class BbvViewportBridge {
public:
    explicit BbvViewportBridge(rhi::IDevice& device, Int3 dimensions = {16,16,16});
    BbvViewportBridge(const BbvViewportBridge&) = delete;
    BbvViewportBridge& operator=(const BbvViewportBridge&) = delete;

    // The caller must have published source edits before invoking advance().
    // sourceRevision must change for every occupancy-affecting scene revision.
    // When keysComplete=false, every in-range brick is reconciled on revision
    // change; true uses the supplied changedKeys (must be COMPLETE for revision).
    // worldOriginMeters is added to camera pose only when it represents a
    // floating-origin-relative camera position. Otherwise leave it zero.
    [[nodiscard]] bool advance(const CameraFramePlan& plan,
                               const PackedBrickmapScene& source,
                               float metresPerVoxel,
                               std::uint64_t sourceRevision,
                               std::span<const BrickKey> changedKeys = {},
                               bool keysComplete = false,
                               Float3 worldOriginMeters = {},
                               std::string* error = nullptr);
    [[nodiscard]] const BbvClipmap* clipmap(camera::CameraViewportId id) const noexcept;
    [[nodiscard]] rhi::BufferHandle buffer(camera::CameraViewportId id) const noexcept;
    [[nodiscard]] bool reset_radiance_history(camera::CameraViewportId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return viewports_.size(); }
    void reset() noexcept;

private:
    struct View {
        std::unique_ptr<BbvClipmap> clipmap;
        std::unique_ptr<BbvRhiMirror> mirror;
        std::uint64_t previousSourceRevision{};
        bool hasRevision{};
        bool resetHistory{true};
    };
    rhi::IDevice& device_;
    Int3 dimensions_{};
    std::map<camera::CameraViewportId, View> viewports_;
};

} // namespace dve::render::rdv
