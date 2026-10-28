#include "dve/xr/openxr_configuration.hpp"
#include <algorithm>
namespace dve::xr {
bool query_stereo_view_configuration(XrInstance instance,XrSystemId system,
    PFN_xrEnumerateViewConfigurationViews enumerate,std::array<XrViewConfigurationView,2>& out,std::string& error) {
    out={};error.clear();
    if(instance==XR_NULL_HANDLE || system==XR_NULL_SYSTEM_ID || !enumerate) {error="OpenXR view configuration inputs missing";return false;}
    for(auto& v:out) v.type=XR_TYPE_VIEW_CONFIGURATION_VIEW;
    uint32_t count=0;
    const auto result=enumerate(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,out.data());
    if(XR_FAILED(result) || count!=2) {out={};error="OpenXR primary stereo view configuration unavailable (XrResult "+std::to_string(result)+", count "+std::to_string(count)+")";return false;}
    for(const auto& v:out) {
        if(!v.recommendedImageRectWidth || !v.recommendedImageRectHeight || !v.recommendedSwapchainSampleCount ||
           v.recommendedImageRectWidth>v.maxImageRectWidth || v.recommendedImageRectHeight>v.maxImageRectHeight ||
           v.recommendedSwapchainSampleCount>v.maxSwapchainSampleCount) {
            out={};error="OpenXR returned invalid recommended stereo image dimensions or sample count";return false;
        }
    }
    return true;
}
bool choose_swapchain_format(XrSession session,PFN_xrEnumerateSwapchainFormats enumerate,
    const std::vector<int64_t>& preferred,int64_t& selected,std::string& error) {
    selected=0;error.clear();
    if(session==XR_NULL_HANDLE || !enumerate || preferred.empty()) {error="OpenXR swapchain format inputs missing";return false;}
    uint32_t count=0; auto result=enumerate(session,0,&count,nullptr);
    if(XR_FAILED(result) || !count || count>4096) {error="OpenXR swapchain format query failed or returned invalid count (XrResult "+std::to_string(result)+")";return false;}
    std::vector<int64_t> available(count);uint32_t written=count;
    result=enumerate(session,count,&written,available.data());
    if(XR_FAILED(result) || !written || written>count) {error="OpenXR swapchain format enumeration failed (XrResult "+std::to_string(result)+")";return false;}
    available.resize(written);
    for(auto candidate:preferred) if(std::find(available.begin(),available.end(),candidate)!=available.end()) {selected=candidate;return true;}
    error="OpenXR runtime supports none of the requested swapchain formats";return false;
}
}
