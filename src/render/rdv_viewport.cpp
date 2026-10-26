#include "dve/render/rdv_viewport.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace dve::render::rdv {
namespace {
bool finite3(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

BbvViewportBridge::BbvViewportBridge(rhi::IDevice& device, Int3 dimensions)
    : device_(device), dimensions_(dimensions) {
    // Delegate the dimension contract to the single authoritative validator.
    (void)BbvClipmap(dimensions_);
}

bool BbvViewportBridge::advance(const CameraFramePlan& plan,
                                const PackedBrickmapScene& source,
                                float metresPerVoxel,
                                std::uint64_t sourceRevision,
                                std::span<const BrickKey> changedKeys,
                                bool keysComplete,
                                Float3 worldOriginMeters,
                                std::string* error) {
    if (!(metresPerVoxel > 0.0F) || !std::isfinite(metresPerVoxel) ||
        !finite3(worldOriginMeters) || plan.viewports.size() > 16U) {
        if (error) *error = "RDV viewport bridge received invalid scale, origin or viewport count";
        return false;
    }
    std::set<camera::CameraViewportId> seen;
    for (const auto& planView : plan.viewports) {
        const auto id = planView.frame.viewport.id;
        if (!seen.insert(id).second) {
            if (error) *error = "RDV viewport bridge received duplicate viewport IDs";
            return false;
        }
        const auto position = add(planView.frame.pose.position, worldOriginMeters);
        if (!finite3(position)) {
            if (error) *error = "RDV viewport camera position must be finite";
            return false;
        }
    }

    try {
        for (const auto& planView : plan.viewports) {
            const auto id = planView.frame.viewport.id;
            auto& view = viewports_[id];
            const bool created = !view.clipmap;
            if (created) {
                view.clipmap = std::make_unique<BbvClipmap>(dimensions_);
                view.mirror = std::make_unique<BbvRhiMirror>(device_);
                view.resetHistory = true;
            }
            const Float3 worldPosition = add(planView.frame.pose.position, worldOriginMeters);
            const auto scroll = view.clipmap->recenter_camera(worldPosition, metresPerVoxel, source);
            const bool changedRevision = !view.hasRevision || view.previousSourceRevision != sourceRevision;
            if (changedRevision) {
                if (keysComplete && !created) {
                    (void)view.clipmap->refresh(changedKeys, source);
                } else if (!created) {
                    (void)view.clipmap->refresh_all(source);
                }
            }
            // Camera cuts reset indirect light history, but should not trigger an
            // unnecessary complete occupancy upload when the clipmap is unchanged.
            view.resetHistory = created || planView.history.resetOcclusion ||
                                planView.history.resetTaa || scroll.fullRefresh;
            if (!view.mirror->publish(*view.clipmap, error)) return false;
            view.previousSourceRevision = sourceRevision;
            view.hasRevision = true;
        }
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }

    // Only after every viewport has published successfully may we retire
    // previous-frame viewports. Failure leaves the previous set retryable.
    for (auto it = viewports_.begin(); it != viewports_.end();) {
        if (!seen.contains(it->first)) it = viewports_.erase(it);
        else ++it;
    }
    return true;
}

const BbvClipmap* BbvViewportBridge::clipmap(camera::CameraViewportId id) const noexcept {
    const auto it = viewports_.find(id);
    return it == viewports_.end() ? nullptr : it->second.clipmap.get();
}
rhi::BufferHandle BbvViewportBridge::buffer(camera::CameraViewportId id) const noexcept {
    const auto it = viewports_.find(id);
    return it == viewports_.end() || !it->second.mirror
        ? rhi::BufferHandle{} : it->second.mirror->buffer();
}
bool BbvViewportBridge::reset_radiance_history(camera::CameraViewportId id) const noexcept {
    const auto it = viewports_.find(id);
    return it != viewports_.end() && it->second.resetHistory;
}
void BbvViewportBridge::reset() noexcept {
    viewports_.clear();
}

} // namespace dve::render::rdv
