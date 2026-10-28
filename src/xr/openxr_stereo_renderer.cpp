#include "dve/xr/openxr_stereo_renderer.hpp"
#include <exception>
namespace dve::xr {
StereoRenderer::StereoRenderer(XrSession session,XrSpace space,PFN_xrLocateViews locate,
        std::array<EyeTarget,2> targets,EyeRenderer& renderer)
    : session_(session),space_(space),locate_(locate),targets_(targets),renderer_(renderer) {}
bool StereoRenderer::render(XrTime time,std::vector<const XrCompositionLayerBaseHeader*>& layers,
                            std::string& error) {
    layers.clear(); error.clear();
    if (poisoned_) { error="OpenXR stereo renderer requires teardown after an image lifecycle error"; return false; }
    if (!targets_[0].swapchain || !targets_[1].swapchain ||
        !targets_[0].swapchain->get() || !targets_[1].swapchain->get() ||
        targets_[0].swapchain->get()==targets_[1].swapchain->get()) {
        error="OpenXR stereo renderer requires two distinct live swapchains"; return false;
    }
    for (const auto& target:targets_) {
        if (target.imageRect.offset.x<0 || target.imageRect.offset.y<0 ||
            target.imageRect.extent.width<=0 || target.imageRect.extent.height<=0) {
            error="OpenXR stereo target has an invalid image rectangle"; return false;
        }
    }
    StereoViews views;
    const auto location=locate_stereo_views(session_,space_,time,locate_,views,error);
    if (location!=LocateResult::Valid) {
        if (location==LocateResult::Untracked) error="OpenXR stereo tracking unavailable";
        return false;
    }
    projectionViews_={}; projection_={};
    for (uint32_t eye=0;eye<2;++eye) {
        auto& target=targets_[eye]; uint32_t imageIndex=0;
        if (!target.swapchain->acquire(imageIndex,error)) return false;
        const auto wait=target.swapchain->wait(XR_INFINITE_DURATION,error);
        if (wait!=WaitResult::Ready) {
            poisoned_=true;
            if (wait==WaitResult::Timeout) error="OpenXR swapchain wait timed out with an image still acquired";
            return false;
        }
        std::string drawError;
        bool drawn=false;
        try { drawn=renderer_.draw_eye(eye,imageIndex,views.views[eye],target,drawError); }
        catch (const std::exception& e) {
            poisoned_=true; error=std::string("OpenXR eye renderer threw; image remains acquired: ")+e.what(); return false;
        } catch (...) {
            poisoned_=true; error="OpenXR eye renderer threw; image remains acquired"; return false;
        }
        std::string releaseError;
        if (!target.swapchain->release(releaseError)) {
            poisoned_=true;
            error=drawn ? releaseError : (drawError.empty()?"OpenXR eye rendering failed":drawError)+"; image release failed: "+releaseError;
            return false;
        }
        if (!drawn) {error=drawError.empty()?"OpenXR eye rendering failed":drawError; return false;}
        auto& v=projectionViews_[eye]; v.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        v.pose=views.views[eye].pose; v.fov=views.views[eye].fov;
        v.subImage.swapchain=target.swapchain->get(); v.subImage.imageRect=target.imageRect;
        v.subImage.imageArrayIndex=target.imageArrayIndex;
    }
    projection_.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION; projection_.space=space_;
    projection_.viewCount=static_cast<uint32_t>(projectionViews_.size()); projection_.views=projectionViews_.data();
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection_));
    return true;
}
}
