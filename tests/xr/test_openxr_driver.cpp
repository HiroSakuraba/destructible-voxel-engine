#include "dve/xr/openxr_driver.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <vector>
using namespace dve::xr;
static std::vector<int> calls;
static bool shouldRender=true;
static uint32_t lastLayers=99;
static XrTime lastTime=0;
static XrResult XRAPI_PTR bs(XrSession, const XrSessionBeginInfo* i) { assert(i->type==XR_TYPE_SESSION_BEGIN_INFO); calls.push_back(1); return XR_SUCCESS; }
static XrResult XRAPI_PTR es(XrSession) { calls.push_back(2); return XR_SUCCESS; }
static XrResult XRAPI_PTR wf(XrSession, const XrFrameWaitInfo* i, XrFrameState* s) { assert(i->type==XR_TYPE_FRAME_WAIT_INFO); s->predictedDisplayTime=123; s->shouldRender=shouldRender?XR_TRUE:XR_FALSE; calls.push_back(3); return XR_SUCCESS; }
static XrResult XRAPI_PTR bf(XrSession, const XrFrameBeginInfo*) { calls.push_back(4); return XR_SUCCESS; }
static XrResult XRAPI_PTR ef(XrSession, const XrFrameEndInfo* i) { calls.push_back(5); lastLayers=i->layerCount; lastTime=i->displayTime; assert(i->environmentBlendMode==XR_ENVIRONMENT_BLEND_MODE_OPAQUE); if(i->layerCount) assert(i->layers && i->layers[0]); return XR_SUCCESS; }
struct Renderer : OpenXrRenderer {
    XrCompositionLayerProjection layer{};
    bool fail{}; bool empty{};
    bool render(XrTime time,std::vector<const XrCompositionLayerBaseHeader*>& layers,std::string& error) override {
        assert(time==123); calls.push_back(6); if(fail) { error="render failure"; return false; }
        if(!empty) { layer.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION; layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)); } return true;
    }
};
int main() {
    Renderer renderer; OpenXrFunctions f{bs,es,wf,bf,ef};
    OpenXrDriver driver(reinterpret_cast<XrSession>(1), f, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, renderer);
    SessionLoop loop(driver); std::string error; SessionState state;
    assert(session_state_from_openxr(XR_SESSION_STATE_READY,state) && state==SessionState::Ready);
    assert(!session_state_from_openxr(XR_SESSION_STATE_UNKNOWN,state));
    assert(loop.on_state(state,error)); calls.clear();
    assert(loop.frame(error)==FrameOutcome::Submitted && lastLayers==1 && lastTime==123);
    assert((calls==std::vector<int>{3,4,6,5}));
    calls.clear(); shouldRender=false;
    assert(loop.frame(error)==FrameOutcome::Empty && lastLayers==0 && lastTime==123);
    assert((calls==std::vector<int>{3,4,5}));
    shouldRender=true; renderer.fail=true; calls.clear();
    assert(loop.frame(error)==FrameOutcome::Failed && error=="render failure" && lastLayers==0);
    assert((calls==std::vector<int>{3,4,6,5}));
    renderer.fail=false; renderer.empty=true;
    assert(loop.frame(error)==FrameOutcome::Failed && lastLayers==0);
    assert(loop.on_state(SessionState::Stopping,error) && calls.back()==2);
    OpenXrDriver missing(XR_NULL_HANDLE, {}, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, renderer);
    assert(!missing.begin_session(error) && error.find("missing session")!=std::string::npos);
    std::cout << "OpenXR driver contract tests passed\n";
}
