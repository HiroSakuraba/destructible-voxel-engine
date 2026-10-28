#pragma once
#include <openxr/openxr.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace dve::xr {
// These helpers require the caller to first confirm primary stereo is supported.
// Requested formats are native graphics-API format values, in preference order.
bool query_stereo_view_configuration(XrInstance instance,XrSystemId system,
    PFN_xrEnumerateViewConfigurationViews enumerate,
    std::array<XrViewConfigurationView,2>& views,std::string& error);
bool choose_swapchain_format(XrSession session,PFN_xrEnumerateSwapchainFormats enumerate,
    const std::vector<int64_t>& preferred,int64_t& selected,std::string& error);
}
