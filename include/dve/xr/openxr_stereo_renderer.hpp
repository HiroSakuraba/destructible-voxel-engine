#pragma once
#include "dve/xr/openxr_driver.hpp"
#include "dve/xr/openxr_resources.hpp"
#include "dve/xr/openxr_views.hpp"
#include <array>
namespace dve::xr {
struct EyeTarget {
    SwapchainHandle* swapchain{};
    XrRect2Di imageRect{};
    uint32_t imageArrayIndex{};
};
struct EyeRenderer {
    virtual ~EyeRenderer() = default;
    // Draw to the acquired image, using view.pose/fov and the selected image subrect.
    // Before returning (including failure), GPU work that uses this image must be
    // complete and all graphics-API layout/ownership requirements must be satisfied.
    // The image index is local to target.swapchain and must be bounds-checked against
    // the caller's previously enumerated images.
    virtual bool draw_eye(uint32_t eye, uint32_t imageIndex, const XrView& view,
                          const EyeTarget& target, std::string& error) = 0;
};
// Creates projection layers from runtime-tracked views and two runtime swapchains.
// This does not create a graphics device or draw geometry; the caller supplies both.
// All borrowed session, space, swapchain and renderer objects must outlive this object.
class StereoRenderer final : public OpenXrRenderer {
public:
    StereoRenderer(XrSession session, XrSpace space, PFN_xrLocateViews locate,
                   std::array<EyeTarget,2> targets, EyeRenderer& renderer);
    bool render(XrTime time, std::vector<const XrCompositionLayerBaseHeader*>& layers,
                std::string& error) override;
    // A failed wait/release or thrown draw callback can leave an image outstanding. No further
    // rendering should be attempted; the owning application must tear down safely.
    bool poisoned() const noexcept { return poisoned_; }
private:
    XrSession session_;
    XrSpace space_;
    PFN_xrLocateViews locate_;
    std::array<EyeTarget,2> targets_;
    EyeRenderer& renderer_;
    std::array<XrCompositionLayerProjectionView,2> projectionViews_{};
    XrCompositionLayerProjection projection_{};
    bool poisoned_{};
};
}
