#include "dve/xr/openxr_stereo_renderer.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace dve::xr;
static int acquired=0,waited=0,released=0; static bool failRelease=false,untracked=false,failDraw=false,failWait=false,throwDraw=false;
static XrResult XRAPI_PTR create(XrSession,const XrSwapchainCreateInfo*,XrSwapchain* h) {static uintptr_t next=10;*h=reinterpret_cast<XrSwapchain>(next++);return XR_SUCCESS;}
static XrResult XRAPI_PTR destroy(XrSwapchain) {return XR_SUCCESS;}
static XrResult XRAPI_PTR acquire(XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t* index) {++acquired;*index=2;return XR_SUCCESS;}
static XrResult XRAPI_PTR wait(XrSwapchain,const XrSwapchainImageWaitInfo* info) {assert(info->timeout==XR_INFINITE_DURATION);++waited;return failWait?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;}
static XrResult XRAPI_PTR release(XrSwapchain,const XrSwapchainImageReleaseInfo*) {++released;return failRelease?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;}
static XrResult XRAPI_PTR locate(XrSession,const XrViewLocateInfo* info,XrViewState* state,uint32_t n,uint32_t* count,XrView* views) {
    assert(info->displayTime==123 && n==2);*count=2;state->viewStateFlags=untracked?0:XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    views[0].pose.position.x=-.03f;views[1].pose.position.x=.03f;return XR_SUCCESS;
}
static XrResult XRAPI_PTR beginSession(XrSession,const XrSessionBeginInfo*) {return XR_SUCCESS;}
static XrResult XRAPI_PTR endSession(XrSession) {return XR_SUCCESS;}
static XrResult XRAPI_PTR waitFrame(XrSession,const XrFrameWaitInfo*,XrFrameState* s) {s->predictedDisplayTime=123;s->shouldRender=XR_TRUE;return XR_SUCCESS;}
static XrResult XRAPI_PTR beginFrame(XrSession,const XrFrameBeginInfo*) {return XR_SUCCESS;}
static XrResult XRAPI_PTR endFrame(XrSession,const XrFrameEndInfo* info) {
    assert(info->displayTime==123 && info->layerCount==1 && info->layers);
    auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
    assert(p->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION && p->viewCount==2);return XR_SUCCESS;
}
struct Draw : EyeRenderer {int calls{};
    bool draw_eye(uint32_t eye,uint32_t index,const XrView& v,const EyeTarget&,std::string& error) override {
        ++calls;assert(index==2 && (eye==0?v.pose.position.x<0:v.pose.position.x>0));
        if(throwDraw) throw std::runtime_error("draw exception");
        if(failDraw) {error="draw failed";return false;} return true;
    }
};
int main() {
    auto session=reinterpret_cast<XrSession>(3);auto space=reinterpret_cast<XrSpace>(4);
    XrSwapchainCreateInfo info{};info.type=XR_TYPE_SWAPCHAIN_CREATE_INFO;
    SwapchainFunctions fn{create,destroy,acquire,wait,release};SwapchainHandle left,right;std::string error;
    assert(left.create(session,info,fn,error) && right.create(session,info,fn,error));
    XrRect2Di rect{{0,0},{100,80}}; std::array<EyeTarget,2> targets{{{&left,rect,0},{&right,rect,0}}};
    Draw draw;StereoRenderer renderer(session,space,locate,targets,draw);
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    assert(renderer.render(123,layers,error) && layers.size()==1 && !renderer.poisoned());
    const auto* layer=reinterpret_cast<const XrCompositionLayerProjection*>(layers[0]);
    assert(layer->viewCount==2 && layer->space==space && layer->views[0].subImage.swapchain==left.get());
    assert(layer->views[1].subImage.swapchain==right.get() && acquired==2 && waited==2 && released==2);
    OpenXrFunctions funcs{beginSession,endSession,waitFrame,beginFrame,endFrame};
    OpenXrDriver driver(session,funcs,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,XR_ENVIRONMENT_BLEND_MODE_OPAQUE,renderer);
    SessionLoop loop(driver);assert(loop.on_state(SessionState::Ready,error));
    assert(loop.frame(error)==FrameOutcome::Submitted);
    assert(loop.on_state(SessionState::Stopping,error));
    assert(acquired==4 && released==4);
    untracked=true;assert(!renderer.render(123,layers,error) && layers.empty() && acquired==4 && !renderer.poisoned());
    untracked=false;failDraw=true;assert(!renderer.render(123,layers,error) && error=="draw failed");
    assert(!left.acquired() && !renderer.poisoned() && released==5);
    failDraw=false;failRelease=true;assert(!renderer.render(123,layers,error) && renderer.poisoned() && left.acquired());
    assert(!renderer.render(123,layers,error));
    failRelease=false;assert(left.release(error));assert(left.close(error) && right.close(error));
    SwapchainHandle a,b;assert(a.create(session,info,fn,error) && b.create(session,info,fn,error));
    targets={{{&a,rect,0},{&b,rect,0}}};StereoRenderer badWait(session,space,locate,targets,draw);
    failWait=true;assert(!badWait.render(123,layers,error) && badWait.poisoned() && a.acquired());
    failWait=false;assert(a.wait(XR_INFINITE_DURATION,error)==WaitResult::Ready);assert(a.release(error));
    assert(a.close(error) && b.close(error));
    SwapchainHandle c,d; assert(c.create(session,info,fn,error) && d.create(session,info,fn,error));
    targets={{{&c,rect,0},{&d,rect,0}}}; StereoRenderer throwing(session,space,locate,targets,draw);
    throwDraw=true; assert(!throwing.render(123,layers,error) && throwing.poisoned() && c.acquired());
    throwDraw=false; assert(c.release(error) && c.close(error) && d.close(error));
    std::cout << "OpenXR stereo renderer contract tests passed\n";
}
