#include "dve/xr/openxr_driver.hpp"
#include <limits>
namespace dve::xr {
namespace {
bool checked(XrResult result, const char* operation, std::string& error) {
    if (XR_SUCCEEDED(result)) return true;
    error = std::string(operation) + " failed (XrResult " + std::to_string(result) + ")";
    return false;
}

}
OpenXrDriver::OpenXrDriver(XrSession session, OpenXrFunctions functions,
        XrViewConfigurationType viewType, XrEnvironmentBlendMode blendMode, OpenXrRenderer& renderer)
    : session_(session), fn_(functions), viewType_(viewType), blendMode_(blendMode), renderer_(renderer) {}
bool OpenXrDriver::begin_session(std::string& error) {
    if (session_ == XR_NULL_HANDLE || !fn_.beginSession) { error="OpenXR driver missing session or xrBeginSession"; return false; }
    XrSessionBeginInfo info{}; info.type=XR_TYPE_SESSION_BEGIN_INFO; info.primaryViewConfigurationType=viewType_;
    return checked(fn_.beginSession(session_, &info), "xrBeginSession", error);
}
bool OpenXrDriver::end_session(std::string& error) {
    if (session_ == XR_NULL_HANDLE || !fn_.endSession) { error="OpenXR driver missing session or xrEndSession"; return false; }
    return checked(fn_.endSession(session_), "xrEndSession", error);
}
bool OpenXrDriver::wait_frame(FrameTiming& timing, std::string& error) {
    if (session_ == XR_NULL_HANDLE || !fn_.waitFrame) { error="OpenXR driver missing session or xrWaitFrame"; return false; }
    XrFrameWaitInfo info{}; info.type=XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState state{}; state.type=XR_TYPE_FRAME_STATE;
    if (!checked(fn_.waitFrame(session_, &info, &state), "xrWaitFrame", error)) return false;
    timing.predictedDisplayTime=state.predictedDisplayTime; timing.shouldRender=state.shouldRender==XR_TRUE;
    return true;
}
bool OpenXrDriver::begin_frame(std::string& error) {
    if (session_ == XR_NULL_HANDLE || !fn_.beginFrame) { error="OpenXR driver missing session or xrBeginFrame"; return false; }
    XrFrameBeginInfo info{}; info.type=XR_TYPE_FRAME_BEGIN_INFO;
    return checked(fn_.beginFrame(session_, &info), "xrBeginFrame", error);
}
bool OpenXrDriver::render_frame(const FrameTiming& timing, std::string& error) {
    layers_.clear();
    if (!renderer_.render(timing.predictedDisplayTime, layers_, error)) { layers_.clear(); return false; }
    if (layers_.empty() || layers_.size() > std::numeric_limits<uint32_t>::max()) {
        error="OpenXR renderer returned an invalid layer count"; layers_.clear(); return false;
    }
    for (const auto* layer : layers_) if (!layer) { error="OpenXR renderer returned a null layer"; layers_.clear(); return false; }
    return true;
}
bool OpenXrDriver::end_frame(const FrameTiming& timing, bool submitLayers, std::string& error) {
    if (session_ == XR_NULL_HANDLE || !fn_.endFrame) { layers_.clear(); error="OpenXR driver missing session or xrEndFrame"; return false; }
    XrFrameEndInfo info{}; info.type=XR_TYPE_FRAME_END_INFO; info.displayTime=timing.predictedDisplayTime;
    info.environmentBlendMode=blendMode_;
    if (submitLayers) { info.layerCount=static_cast<uint32_t>(layers_.size()); info.layers=layers_.data(); }
    const auto result=fn_.endFrame(session_, &info); layers_.clear();
    return checked(result, "xrEndFrame", error);
}
bool session_state_from_openxr(XrSessionState input, SessionState& output) noexcept {
    switch(input) {
    case XR_SESSION_STATE_IDLE: output=SessionState::Idle; break;
    case XR_SESSION_STATE_READY: output=SessionState::Ready; break;
    case XR_SESSION_STATE_SYNCHRONIZED: output=SessionState::Synchronized; break;
    case XR_SESSION_STATE_VISIBLE: output=SessionState::Visible; break;
    case XR_SESSION_STATE_FOCUSED: output=SessionState::Focused; break;
    case XR_SESSION_STATE_STOPPING: output=SessionState::Stopping; break;
    case XR_SESSION_STATE_LOSS_PENDING: output=SessionState::LossPending; break;
    case XR_SESSION_STATE_EXITING: output=SessionState::Exiting; break;
    default: return false;
    }
    return true;
}
}
