#include "dve/xr/openxr_views.hpp"
namespace dve::xr {
LocateResult locate_stereo_views(XrSession session, XrSpace space, XrTime displayTime,
    PFN_xrLocateViews locate, StereoViews& output, std::string& error) {
    output={}; error.clear();
    if (session==XR_NULL_HANDLE || space==XR_NULL_HANDLE || !locate || displayTime<=0) {
        error="OpenXR view location requires a session, space, function and positive display time"; return LocateResult::Failed;
    }
    XrViewLocateInfo info{}; info.type=XR_TYPE_VIEW_LOCATE_INFO;
    info.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; info.displayTime=displayTime; info.space=space;
    XrViewState state{}; state.type=XR_TYPE_VIEW_STATE;
    for (auto& view : output.views) view.type=XR_TYPE_VIEW;
    uint32_t count=0;
    const auto result=locate(session,&info,&state,static_cast<uint32_t>(output.views.size()),&count,output.views.data());
    if (XR_FAILED(result)) { output={}; error="xrLocateViews failed (XrResult "+std::to_string(result)+")"; return LocateResult::Failed; }
    if (count!=output.views.size()) { output={}; error="OpenXR primary stereo did not return two views"; return LocateResult::Failed; }
    output.flags=state.viewStateFlags;
    constexpr XrViewStateFlags required=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((state.viewStateFlags & required)!=required) { output={}; return LocateResult::Untracked; }
    return LocateResult::Valid;
}
}
