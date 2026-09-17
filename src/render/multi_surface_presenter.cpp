#include "dve/render/multi_surface_presenter.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

bool equivalent(const PresentationSurfaceRequest& left,
                const PresentationSurfaceRequest& right) noexcept {
    return left.nativeWindow.backend == right.nativeWindow.backend &&
           left.nativeWindow.window == right.nativeWindow.window &&
           left.nativeWindow.display == right.nativeWindow.display &&
           left.nativeWindow.auxiliary == right.nativeWindow.auxiliary &&
           left.width == right.width && left.height == right.height &&
           left.format == right.format && left.presentMode == right.presentMode &&
           left.imageCount == right.imageCount && left.debugName == right.debugName;
}
}

bool PresentationSurfaceRequest::validate(std::string* error) const {
    if (window == presentation::kInvalidWindowId || width == 0U || height == 0U ||
        width > 16384U || height > 16384U || imageCount < 2U || imageCount > 4U) {
        set_error(error, "presentation surface request has invalid identity, dimensions, or image count");
        return false;
    }
    return true;
}

bool MultiSurfacePresenter::reconcile(rhi::IDevice& device,
                                      std::span<const PresentationSurfaceRequest> requests,
                                      std::string* error) {
    std::set<presentation::WindowId> requested;
    for (const auto& request : requests) {
        if (!request.validate(error)) return false;
        if (!requested.insert(request.window).second) {
            set_error(error, "presentation surface requests contain duplicate window IDs");
            return false;
        }
    }

    for (auto iterator = surfaces_.begin(); iterator != surfaces_.end();) {
        if (!requested.contains(iterator->first)) {
            if (iterator->second.swapchain)
                (void)device.destroy_swapchain(iterator->second.swapchain, nullptr);
            iterator = surfaces_.erase(iterator);
        } else ++iterator;
    }

    for (const auto& request : requests) {
        auto found = surfaces_.find(request.window);
        const bool recreate = found == surfaces_.end() || found->second.needsRecreate ||
                              !equivalent(found->second.request, request);
        if (!recreate) continue;
        std::uint64_t generation = 1U;
        if (found != surfaces_.end()) {
            generation = found->second.generation + 1U;
            if (found->second.hasAcquired) {
                set_error(error, "cannot recreate a surface while an image is acquired");
                return false;
            }
            if (found->second.swapchain && !device.destroy_swapchain(found->second.swapchain, error))
                return false;
            surfaces_.erase(found);
        }
        rhi::SwapchainDesc desc;
        desc.window = request.nativeWindow;
        desc.format = request.format;
        desc.width = request.width;
        desc.height = request.height;
        desc.imageCount = request.imageCount;
        desc.presentMode = request.presentMode;
        desc.debugName = request.debugName;
        const auto swapchain = device.create_swapchain(desc, error);
        if (!swapchain) return false;
        Surface surface;
        surface.request = request;
        surface.swapchain = swapchain;
        surface.generation = generation;
        surfaces_.emplace(request.window, std::move(surface));
    }
    return true;
}

std::vector<AcquiredPresentationSurface> MultiSurfacePresenter::acquire_all(rhi::IDevice& device,
                                                                            std::string* error) {
    std::vector<AcquiredPresentationSurface> result;
    result.reserve(surfaces_.size());
    for (auto& [window, surface] : surfaces_) {
        if (surface.hasAcquired) {
            set_error(error, "presentation surface already has an acquired image");
            return {};
        }
        surface.acquired = device.acquire_next_image(surface.swapchain, error);
        if (!surface.acquired.texture) return {};
        surface.hasAcquired = true;
        result.push_back({window, surface.swapchain, surface.acquired.texture,
                          surface.acquired.imageIndex, surface.acquired.suboptimal});
    }
    return result;
}

MultiSurfacePresentReport MultiSurfacePresenter::present_all(
    rhi::IDevice& device,
    std::span<const std::pair<presentation::WindowId, rhi::FenceHandle>> waitFences,
    std::string* error) {
    std::map<presentation::WindowId, rhi::FenceHandle> fences;
    for (const auto& [window, fence] : waitFences) fences[window] = fence;
    MultiSurfacePresentReport report;
    for (auto& [window, surface] : surfaces_) {
        if (!surface.hasAcquired) {
            ++report.errors;
            set_error(error, "presentation surface has no acquired image");
            continue;
        }
        const auto found = fences.find(window);
        if (found == fences.end() || !found->second) {
            ++report.errors;
            set_error(error, "presentation surface is missing a wait fence");
            continue;
        }
        const auto present = device.present(surface.swapchain, found->second, error);
        surface.hasAcquired = false;
        surface.acquired = {};
        switch (present) {
            case rhi::PresentResult::Presented: ++report.presented; break;
            case rhi::PresentResult::Suboptimal:
                ++report.suboptimal; surface.needsRecreate = true; break;
            case rhi::PresentResult::OutOfDate:
                ++report.outOfDate; surface.needsRecreate = true; break;
            case rhi::PresentResult::DeviceLost: ++report.deviceLost; break;
            case rhi::PresentResult::Error: ++report.errors; break;
        }
    }
    return report;
}

bool MultiSurfacePresenter::mark_out_of_date(presentation::WindowId window) noexcept {
    const auto found = surfaces_.find(window);
    if (found == surfaces_.end()) return false;
    found->second.needsRecreate = true;
    return true;
}

void MultiSurfacePresenter::shutdown(rhi::IDevice& device) noexcept {
    for (auto& [window, surface] : surfaces_) {
        (void)window;
        if (surface.swapchain) (void)device.destroy_swapchain(surface.swapchain, nullptr);
    }
    surfaces_.clear();
}

std::vector<PresentationSurfaceStatus> MultiSurfacePresenter::statuses() const {
    std::vector<PresentationSurfaceStatus> result;
    result.reserve(surfaces_.size());
    for (const auto& [window, surface] : surfaces_) {
        result.push_back({window, surface.request.width, surface.request.height,
                          surface.generation, surface.hasAcquired, surface.needsRecreate});
    }
    return result;
}

} // namespace dve::render
