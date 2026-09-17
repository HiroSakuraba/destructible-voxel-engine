#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "dve/display_presentation.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct PresentationSurfaceRequest {
    presentation::WindowId window{presentation::kInvalidWindowId};
    platform::NativeWindowHandle nativeWindow{};
    std::uint32_t width{};
    std::uint32_t height{};
    rhi::TextureFormat format{rhi::TextureFormat::BGRA8Unorm};
    rhi::PresentMode presentMode{rhi::PresentMode::Fifo};
    std::uint32_t imageCount{3U};
    std::string debugName;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct AcquiredPresentationSurface {
    presentation::WindowId window{presentation::kInvalidWindowId};
    rhi::SwapchainHandle swapchain{};
    rhi::TextureHandle texture{};
    std::uint32_t imageIndex{};
    bool suboptimal{};
};

struct PresentationSurfaceStatus {
    presentation::WindowId window{presentation::kInvalidWindowId};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t generation{};
    bool acquired{};
    bool needsRecreate{};
};

struct MultiSurfacePresentReport {
    std::uint32_t presented{};
    std::uint32_t suboptimal{};
    std::uint32_t outOfDate{};
    std::uint32_t deviceLost{};
    std::uint32_t errors{};
};

class MultiSurfacePresenter {
public:
    MultiSurfacePresenter() = default;
    ~MultiSurfacePresenter() = default;

    MultiSurfacePresenter(const MultiSurfacePresenter&) = delete;
    MultiSurfacePresenter& operator=(const MultiSurfacePresenter&) = delete;

    [[nodiscard]] bool reconcile(rhi::IDevice& device,
                                 std::span<const PresentationSurfaceRequest> requests,
                                 std::string* error = nullptr);
    [[nodiscard]] std::vector<AcquiredPresentationSurface> acquire_all(rhi::IDevice& device,
                                                                       std::string* error = nullptr);
    [[nodiscard]] MultiSurfacePresentReport present_all(
        rhi::IDevice& device,
        std::span<const std::pair<presentation::WindowId, rhi::FenceHandle>> waitFences,
        std::string* error = nullptr);
    [[nodiscard]] bool mark_out_of_date(presentation::WindowId window) noexcept;
    void shutdown(rhi::IDevice& device) noexcept;

    [[nodiscard]] std::vector<PresentationSurfaceStatus> statuses() const;
    [[nodiscard]] std::size_t size() const noexcept { return surfaces_.size(); }

private:
    struct Surface {
        PresentationSurfaceRequest request{};
        rhi::SwapchainHandle swapchain{};
        rhi::AcquiredSwapchainImage acquired{};
        std::uint64_t generation{1U};
        bool hasAcquired{};
        bool needsRecreate{};
    };

    std::map<presentation::WindowId, Surface> surfaces_;
};

} // namespace dve::render
